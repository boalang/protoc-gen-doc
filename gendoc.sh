#!/bin/sh

protofile=$(basename -- "$1")
proto="${protofile%.*}"

export PATH=$PATH:~/boa/protoc-gen-doc/
protoc --doc_out=boa,$proto.inc:. $proto.proto
