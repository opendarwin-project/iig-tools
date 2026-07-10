/*
 * iig-lite - IOKit interface generator (kernel-side subset).
 *
 * Usage mirrors Apple's iig:
 *   iig --def Foo.iig --header Foo.h --impl Foo.iig.cpp
 *
 * Parses the .iig DriverKit interface definition and emits the generated
 * header (Args/Methods/KernelMethods/VirtualMethods macros, RPC message
 * structs) and implementation (Dispatch/Invoke/marshaling wrappers) that a
 * KERNEL build of the class requires. See codegen.cpp for scope notes.
 */
#include "ast.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

static bool
readFile(const std::string &path, std::string &out)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

static bool
writeFile(const std::string &path, const std::string &data)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << data;
  return out.good();
}

int
main(int argc, char **argv)
{
  std::string defPath, headerPath, implPath, frameworkName;

  for (int i = 1; i < argc; i++) {
    auto need = [&](const char *what) -> const char * {
      if (i + 1 >= argc) {
        fprintf(stderr, "iig: %s requires an argument\n", what);
        exit(1);
      }
      return argv[++i];
    };
    if (!strcmp(argv[i], "--def")) defPath = need("--def");
    else if (!strcmp(argv[i], "--header")) headerPath = need("--header");
    else if (!strcmp(argv[i], "--impl")) implPath = need("--impl");
    else if (!strcmp(argv[i], "--framework-name")) frameworkName = need("--framework-name");
    else if (!strcmp(argv[i], "--")) break; /* trailing clang args: ignored */
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      printf("usage: iig --def <file.iig> --header <out.h> --impl <out.iig.cpp>\n");
      return 0;
    }
    /* unknown flags (e.g. -D/-I passthrough) are ignored */
  }

  if (defPath.empty() || headerPath.empty()) {
    fprintf(stderr, "usage: iig --def <file.iig> --header <out.h> [--impl <out.iig.cpp>]\n");
    return 1;
  }

  std::string text;
  if (!readFile(defPath, text)) {
    fprintf(stderr, "iig: cannot read %s\n", defPath.c_str());
    return 1;
  }

  iig::File file;
  file.frameworkName = frameworkName;
  std::string error;
  if (!iig::parseIigFile(defPath, text, file, error)) {
    fprintf(stderr, "iig: %s: %s\n", defPath.c_str(), error.c_str());
    return 1;
  }

  std::string header;
  iig::generateHeader(file, header);
  if (!writeFile(headerPath, header)) {
    fprintf(stderr, "iig: cannot write %s\n", headerPath.c_str());
    return 1;
  }

  if (!implPath.empty()) {
    std::string impl;
    iig::generateImpl(file, impl);
    if (!writeFile(implPath, impl)) {
      fprintf(stderr, "iig: cannot write %s\n", implPath.c_str());
      return 1;
    }
  }
  return 0;
}
