/*
  Copyright 2014, 2015, 2016 Elvis Stansvik

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
*/

#include "mustache.h"

#include <algorithm>
#include <iostream>
#include <string>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVariant>
#include <QVariantHash>
#include <QVariantList>

#include <google/protobuf/compiler/plugin.h>
#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/printer.h>

namespace gp = google::protobuf;
namespace ms = Mustache;

/**
 * Context class for the documentation generator.
 */
class DocGeneratorContext {
public:
    QString template_;      /**< Mustache template, or QString() for raw JSON output */
    QString outputFileName; /**< Output filename. */
    bool noExclude;         /**< Ignore @exclude directives? */
    QVariantList files;     /**< List of files to render. */
};

/// Documentation generator context instance.
static DocGeneratorContext generatorContext;

/**
 * Returns true if the variant @p v1 is less than @p v2.
 *
 * It is assumed that both variants contain a QVariantHash with either
 * a "field_name" or a "value_name" key. This comparator is used
 * when sorting the field and enum value lists.
 */
static inline bool nameLessThan(const QVariant &v1, const QVariant &v2)
{
    if (v1.toHash()["field_name"].toString() < v2.toHash()["field_name"].toString())
        return true;
    return v1.toHash()["value_name"].toString() < v2.toHash()["value_name"].toString();
}

/**
 * Returns the description of the item described by @p descriptor.
 *
 * The item can be a message, enum, enum value, or field.
 *
 * The description is taken as the leading comments followed by the trailing
 * comments. If present, a single space is removed from the start of each line.
 * Whitespace is trimmed from the final result before it is returned.
 * 
 * If the described item should be excluded from the generated documentation,
 * @p exclude is set to true. Otherwise it is set to false.
 */
template<typename T>
static QString descriptionOf(const T *descriptor, bool &excluded)
{
    QString description;

    gp::SourceLocation sourceLocation;
    descriptor->GetSourceLocation(&sourceLocation);

    // Check for leading documentation comments.
    QString leading = QString::fromStdString(sourceLocation.leading_comments);
    if (leading.startsWith('*') || leading.startsWith('/')) {
        leading = leading.mid(1);
        leading.replace(QRegularExpression("^ ", QRegularExpression::MultilineOption), "");
        description += leading;
    }

    // Check for trailing documentation comments.
    QString trailing = QString::fromStdString(sourceLocation.trailing_comments);
    if (trailing.startsWith('*') || trailing.startsWith('/')) {
        trailing = trailing.mid(1);
        trailing.replace(QRegularExpression("^ ", QRegularExpression::MultilineOption), "");
        description += trailing;
    }

    // Check if item should be excluded.
    description = description.trimmed();
    excluded = false;
    if (description.startsWith("@exclude")) {
        description = description.mid(8);
        excluded = !generatorContext.noExclude;
    }

    return description;
}

/**
 * Returns the description of the file described by @p fileDescriptor.
 *
 * If the first non-whitespace characters in the file is a block of consecutive
 * single-line (///) documentation comments, or a multi-line documentation comment,
 * the contents of that block of comments or comment is taken as the description of
 * the file. If a line inside a multi-line comment starts with "* ", " *" or " * "
 * then that prefix is stripped from the line before it is added to the description.
 *
 * If the file has no description, QString() is returned. If an error occurs,
 * @p error is set to point to an error message and QString() is returned.
 * 
 * If the described file should be excluded from the generated documentation,
 * @p exclude is set to true. Otherwise it is set to false.
 */
static QString descriptionOf(const gp::FileDescriptor *fileDescriptor, std::string *error, bool &excluded)
{
    // Since there's no API in gp::FileDescriptor for getting the "file
    // level" comment, we open the file and extract this out ourselves.

    // Open file.
    const QString fileName = QString::fromStdString(fileDescriptor->name());
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QString("%1: %2").arg(fileName).arg(file.errorString()).toStdString();
        return QString();
    }

    // Extract the description.
    QTextStream stream(&file);
    QString description;
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        } else if (line.startsWith("///")) {
            while (!stream.atEnd() && line.startsWith("///")) {
                description += line.mid(line.startsWith("/// ") ? 4 : 3) + '\n';
                line = stream.readLine().trimmed();
            }
            description = description.left(description.size() - 1);
        } else if (line.startsWith("/**") && !line.startsWith("/***/")) {
            line = line.mid(2);
            int start, end;
            while ((end = line.indexOf("*/")) == -1) {
                start = 0;
                if (line.startsWith("*")) ++start;
                if (line.startsWith("* ")) ++start;
                description += line.mid(start) + '\n';
                line = stream.readLine().trimmed();
            }
            start = 0;
            if (line.startsWith("*") && !line.startsWith("*/")) ++start;
            if (line.startsWith("* ")) ++start;
            description += line.mid(start, end - start);
        }
        break;
    }

    // Check if the file should be excluded.
    description = description.trimmed();
    excluded = false;
    if (description.startsWith("@exclude")) {
        description = description.mid(8);
        excluded = !generatorContext.noExclude;
    }

    return description;
}

/**
 * Returns the name of the scalar field type @p type.
 */
static QString scalarTypeName(gp::FieldDescriptor::Type type)
{
    switch (type) {
        case gp::FieldDescriptor::TYPE_BOOL:
            return "<a href=\"/docs/types.php\">bool</a>";
        case gp::FieldDescriptor::TYPE_BYTES:
        case gp::FieldDescriptor::TYPE_STRING:
            return "<a href=\"/docs/types.php\">string</a>";
        case gp::FieldDescriptor::TYPE_DOUBLE:
        case gp::FieldDescriptor::TYPE_FLOAT:
            return "<a href=\"/docs/types.php\">float</a>";
        case gp::FieldDescriptor::TYPE_FIXED32:
        case gp::FieldDescriptor::TYPE_FIXED64:
        case gp::FieldDescriptor::TYPE_INT32:
        case gp::FieldDescriptor::TYPE_INT64:
        case gp::FieldDescriptor::TYPE_SFIXED32:
        case gp::FieldDescriptor::TYPE_SFIXED64:
        case gp::FieldDescriptor::TYPE_SINT32:
        case gp::FieldDescriptor::TYPE_SINT64:
        case gp::FieldDescriptor::TYPE_UINT32:
        case gp::FieldDescriptor::TYPE_UINT64:
            return "<a href=\"/docs/types.php\">int</a>";
        default:
            return "<unknown>";
    }
}

static QString typeUrl(QString type)
{
    return "<a href=\"/docs/dsl-types.php#" + type + "\">" + type + "</a>";
}

/**
 * Add field to variant list.
 *
 * Adds the field described by @p fieldDescriptor to the variant list @p fields.
 */
static void addField(const gp::FieldDescriptor *fieldDescriptor, QVariantList *fields)
{
    bool excluded = false;
    QString description = descriptionOf(fieldDescriptor, excluded);

    if (excluded) {
        return;
    }

    QVariantHash field;

    // Add basic info.
    field["field_name"] = QString::fromStdString(fieldDescriptor->name());
    field["field_description"] = description;

    // Add type information.
    gp::FieldDescriptor::Type type = fieldDescriptor->type();
    QString fieldType;
    if (type == gp::FieldDescriptor::TYPE_MESSAGE || type == gp::FieldDescriptor::TYPE_GROUP) {
        // Field is of message / group type.
        const gp::Descriptor *descriptor = fieldDescriptor->message_type();
        fieldType = typeUrl(QString::fromStdString(descriptor->name()));
    } else if (type == gp::FieldDescriptor::TYPE_ENUM) {
        // Field is of enum type.
        const gp::EnumDescriptor *descriptor = fieldDescriptor->enum_type();
        fieldType = typeUrl(QString::fromStdString(descriptor->name()));
    } else {
        // Field is of scalar type.
        QString typeName(scalarTypeName(type));
        if (QString::fromStdString(fieldDescriptor->name()).indexOf("date") != -1)
            fieldType = "<a href=\"/docs/types.php\">time</a>";
        else
            fieldType = typeName;
    }

    if (fieldDescriptor->label() == gp::FieldDescriptor::LABEL_OPTIONAL)
        fieldType += "?";
    else if (fieldDescriptor->label() == gp::FieldDescriptor::LABEL_REPEATED)
        fieldType = "<a href=\"/docs/types.php\">array</a> of " + fieldType;

    field["field_type"] = fieldType;

    fields->append(field);
}

/**
 * Adds the enum described by @p enumDescriptor to the variant list @p enums.
 */
static void addEnum(const gp::EnumDescriptor *enumDescriptor, QVariantList *enums)
{
    bool excluded = false;
    QString description = descriptionOf(enumDescriptor, excluded);

    if (excluded) {
        return;
    }

    QVariantHash enum_;

    // Add basic info.
    enum_["enum_name"] = QString::fromStdString(enumDescriptor->name());
    enum_["enum_description"] = description;

    // Add enum values.
    QVariantList values;
    for (int i = 0; i < enumDescriptor->value_count(); ++i) {
        const gp::EnumValueDescriptor *valueDescriptor = enumDescriptor->value(i);

        bool excluded = false;
        QString description = descriptionOf(valueDescriptor, excluded);

        if (excluded) {
            continue;
        }

        QVariantHash value;
        value["value_name"] = QString::fromStdString(valueDescriptor->name());
        value["value_number"] = valueDescriptor->number();
        value["value_description"] = description;
        values.append(value);
    }
    std::sort(values.begin(), values.end(), &nameLessThan);
    enum_["enum_values"] = values;

    enums->append(enum_);
}

/**
 * Add messages to variant list.
 *
 * Adds the message described by @p descriptor and all its nested messages and
 * enums to the variant list @p messages and @p enums, respectively.
 */
static void addMessages(const gp::Descriptor *descriptor,
                        QVariantList *messages,
                        QVariantList *enums)
{
    bool excluded = false;
    QString description = descriptionOf(descriptor, excluded);

    if (excluded) {
        return;
    }

    QVariantHash message;

    // Add basic info.
    message["message_name"] = QString::fromStdString(descriptor->name());
    message["message_description"] = description;

    // Add fields.
    QVariantList fields;
    for (int i = 0; i < descriptor->field_count(); ++i) {
        addField(descriptor->field(i), &fields);
    }
    std::sort(fields.begin(), fields.end(), &nameLessThan);
    message["message_has_fields"] = !fields.isEmpty();
    message["message_fields"] = fields;

    messages->append(message);

    // Add nested messages and enums.
    for (int i = 0; i < descriptor->nested_type_count(); ++i) {
        addMessages(descriptor->nested_type(i), messages, enums);
    }
    for (int i = 0; i < descriptor->enum_type_count(); ++i) {
        addEnum(descriptor->enum_type(i), enums);
    }
}

/**
 * Add file to variant list.
 *
 * Adds the file described by @p fileDescriptor to the variant list @p files.
 * If an error occurs, @p error is set to point to an error message and the
 * function returns immediately.
 */
static void addFile(const gp::FileDescriptor *fileDescriptor, QVariantList *files, std::string *error)
{
    bool excluded = false;
    QString description = descriptionOf(fileDescriptor, error, excluded);

    if (excluded) {
        return;
    }

    QVariantHash file;

    // Add basic info.
    file["file_name"] = QFileInfo(QString::fromStdString(fileDescriptor->name())).fileName();
    file["file_description"] = description;
    file["file_package"] = QString::fromStdString(fileDescriptor->package());

    QVariantList messages;
    QVariantList enums;

    // Add messages.
    for (int i = 0; i < fileDescriptor->message_type_count(); ++i) {
        addMessages(fileDescriptor->message_type(i), &messages, &enums);
    }
    file["file_messages"] = messages;

    // Add enums.
    for (int i = 0; i < fileDescriptor->enum_type_count(); ++i) {
        addEnum(fileDescriptor->enum_type(i), &enums);
    }
    file["file_enums"] = enums;

    files->append(file);
}

/**
 * Return a formatted template rendering error.
 *
 * @param template_ Template in which the error occurred.
 * @param renderer Template renderer that failed.
 * @return Formatted single-line error.
 */
static std::string formattedError(const QString &template_, const ms::Renderer &renderer)
{
    QString location = template_;
    if (!renderer.errorPartial().isEmpty()) {
        location += " in partial " + renderer.errorPartial();
    }
    return QString("%1:%2: %3")
            .arg(location)
            .arg(renderer.errorPos())
            .arg(renderer.error()).toStdString();
}

/**
 * Returns the list of formats that are supported out of the box.
 */
static QStringList supportedFormats()
{
    QStringList formats;
    QStringList filter = QStringList() << "*.mustache";
    QFileInfoList entries = QDir(":/templates").entryInfoList(filter);
    for (const QFileInfo &entry : entries) {
        formats.append(entry.baseName());
    }
    return formats;
}

/**
 * Returns a usage help string.
 */
static QString usage()
{
    return QString(
        "Usage: --doc_out=%1|<TEMPLATE_FILENAME>,<OUT_FILENAME>[,no-exclude]:<OUT_DIR>")
        .arg(supportedFormats().join("|"));
}

/**
 * Returns the template specified by @p name.
 *
 * The @p name parameter may be either a template file name, or the name of a
 * supported format ("html", "docbook", ...). If an error occured, @p error is
 * set to point to an error message and QString() returned.
 */
static QString readTemplate(const QString &name, std::string *error)
{
    QString builtInFileName = QString(":/templates/%1.mustache").arg(name);
    QString fileName = supportedFormats().contains(name) ? builtInFileName : name;
    QFile file(fileName);

    if (!file.open(QIODevice::ReadOnly)) {
        *error = QString("%1: %2").arg(fileName).arg(file.errorString()).toStdString();
        return QString();
    } else {
        return file.readAll();
    }
}

/**
 * Parses the plugin parameter string.
 *
 * @param parameter Plugin parameter string.
 * @param error Pointer to error if parsing failed.
 * @return true on success, otherwise false.
 */
static bool parseParameter(const std::string &parameter, std::string *error)
{
    QStringList tokens = QString::fromStdString(parameter).split(",");

    if (tokens.size() != 2 && tokens.size() != 3) {
        *error = usage().toStdString();
        return false;
    }

    bool noExclude = false;
    if (tokens.size() == 3) {
        if (tokens.at(2) == "no-exclude") {
            noExclude = true;
        } else {
            *error = usage().toStdString();
            return false;
        }
    }

    if (tokens.at(0) != "json") {
        generatorContext.template_ = readTemplate(tokens.at(0), error);
    }
    generatorContext.outputFileName = tokens.at(1);
    generatorContext.noExclude = noExclude;

    return true;
}

/**
 * Template filter for breaking paragraphs into HTML `<p>` elements.
 *
 * Renders @p text with @p renderer in @p context and returns the result with
 * paragraphs enclosed in `<p>..</p>`.
 *
 */
static QString pFilter(const QString &text, ms::Renderer* renderer, ms::Context* context)
{
    QRegularExpression re("(\\n|\\r|\\r\\n)\\s*(\\n|\\r|\\r\\n)");
    return "<p>" + renderer->render(text, context).split(re).join("</p><p>") + "</p>";
}

/**
 * Template filter for removing line breaks.
 *
 * Renders @p text with @p renderer in @p context and returns the result with
 * all occurrances of `\r\n`, `\n`, `\r` removed in that order.
 */
static QString nobrFilter(const QString &text, ms::Renderer* renderer, ms::Context* context)
{
    QString result = renderer->render(text, context);
    result.remove("\r\n");
    result.remove("\r");
    result.remove("\n");
    return result;
}

/**
 * Renders the list of files.
 *
 * Renders files to the directory specified in @p context. If an error occurred,
 * @p error is set to point to an error message and no output is written.
 *
 * @param context Compiler generator context specifying the output directory.
 * @param error Pointer to error if rendering failed.
 * @return true on success, otherwise false.
 */
static bool render(gp::compiler::GeneratorContext *context, std::string *error)
{
    QVariantHash args;
    QString result;

    if (generatorContext.template_.isEmpty()) {
        // Raw JSON output.
        QJsonDocument document = QJsonDocument::fromVariant(generatorContext.files);
        if (document.isNull()) {
            *error = "Failed to create JSON document";
            return false;
        }
        result = QString(document.toJson());
    } else {
        // Render using template.

        // Add filters.
        args["p"] = QVariant::fromValue(ms::QtVariantContext::fn_t(pFilter));
        args["nobr"] = QVariant::fromValue(ms::QtVariantContext::fn_t(nobrFilter));

        // Add files list.
        args["files"] = generatorContext.files;

        // Render template.
        ms::Renderer renderer;
        ms::QtVariantContext variantContext(args);
        result = renderer.render(generatorContext.template_, &variantContext);

        // Check for errors.
        if (!renderer.error().isEmpty()) {
            *error = formattedError(generatorContext.template_, renderer);
            return false;
        }
    }

    // Write output.
    std::string outputFileName = generatorContext.outputFileName.toStdString();
    gp::io::ZeroCopyOutputStream *stream = context->Open(outputFileName);
    gp::io::Printer printer(stream, '$');
    printer.PrintRaw(result.toStdString());

    return true;
}

/**
 * Documentation generator class.
 */
class DocGenerator : public gp::compiler::CodeGenerator
{
    /// Implements google::protobuf::compiler::CodeGenerator.
    bool Generate(
            const gp::FileDescriptor *fileDescriptor,
            const std::string &parameter,
            gp::compiler::GeneratorContext *context,
            std::string *error) const
    {
        std::vector<const gp::FileDescriptor *> parsedFiles;
        context->ListParsedFiles(&parsedFiles);
        const bool isFirst = fileDescriptor == parsedFiles.front();
        const bool isLast = fileDescriptor == parsedFiles.back();

        if (isFirst) {
            // Parse the plugin parameter.
            if (!parseParameter(parameter, error)) {
                return false;
            }
        }

        // Parse the file.
        addFile(fileDescriptor, &generatorContext.files, error);
        if (!error->empty()) {
            return false;
        }

        if (isLast) {
            // Render output.
            if (!render(context, error)) {
                return false;
            }
        }

        return true;
    }
};

int main(int argc, char *argv[])
{
    // Instantiate and invoke the generator plugin.
    DocGenerator generator;
    return google::protobuf::compiler::PluginMain(argc, argv, &generator);
}
