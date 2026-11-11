#!/bin/bash

case "$(uname -s)" in
Linux) os=linux ;;
Darwin) os=darwin ;;
*)
  printf 'judge: unsupported operating system: %s\n' "$(uname -s)" >&2
  exit 2
  ;;
esac

case "$(uname -m)" in
x86_64 | amd64) arch=x86_64 ;;
arm64 | aarch64) arch=arm64 ;;
*)
  printf 'judge: unsupported processor architecture: %s\n' "$(uname -m)" >&2
  exit 2
  ;;
esac

printf '%s-%s\n' "$os" "$arch"
