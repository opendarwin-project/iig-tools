/*
 * codegen.cpp , emits the kernel-side generated header and .iig.cpp for a
 * parsed .iig file, imitating Apple's iig output (the checked-in generated
 * files under xnu/iokit/DriverKit are the reference).
 *
 * Scope: everything a KERNEL build needs. The !KERNEL (user/DriverKit.dext)
 * class-metadata section (OSClassDescription, gClassMetaClass, New) is not
 * emitted; nothing in a kext build compiles it.
 *
 * Msgids: Apple's hash is not public; we use FNV-1a over
 * "Class::method(paramtypes)". Self-consistent, which is all the in-kernel
 * dispatch path requires - do NOT mix our generated headers with
 * Apple-generated ones for the same class.
 */
#include "ast.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace iig {

namespace {

/* Types passed by value or as scalar out-pointers (everything else behind a
 * '*' is treated as an OSObject reference). */
bool
isScalarType(const std::string &t)
{
  static const std::set<std::string> scalars = {
    "bool", "char", "short", "int", "long", "unsigned", "signed",
    "size_t", "ssize_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "IOReturn", "kern_return_t", "IOOptionBits", "IOByteCount",
    "mach_port_t", "uint64_t*", /* keyed without spaces below */
  };
  /* strip trailing stars/spaces to get the base */
  std::string base = t;
  while (!base.empty() && (base.back() == '*' || base.back() == ' '))
    base.pop_back();
  if (scalars.count(base)) return true;
  /* OSObject-derived classes that collide with the enum-suffix heuristic
   * below (e.g. "OSAction" ends in "Action" but is a reference type, not a
   * scalar) */
  static const std::set<std::string> objectExceptions = {
    "OSAction", "OSObject",
  };
  if (objectExceptions.count(base)) return false;
  /* enums in the HID headers follow Type/Options suffix naming; treat common
   * integer-ish suffixes as scalar */
  for (const char *suf : { "Type", "Options", "Action" }) {
    size_t l = strlen(suf);
    if (base.size() > l && base.compare(base.size() - l, l, suf) == 0 &&
        base.find('*') == std::string::npos)
      return true;
  }
  return false;
}

int
starCount(const std::string &t)
{
  int n = 0;
  for (char c : t) if (c == '*') n++;
  return n;
}

std::string
baseType(const std::string &t)
{
  std::string b = t;
  /* drop const and stars */
  while (true) {
    if (b.compare(0, 6, "const ") == 0) { b = b.substr(6); continue; }
    break;
  }
  while (!b.empty() && (b.back() == '*' || b.back() == ' ' || b.back() == '&'))
    b.pop_back();
  /* "T * const" style */
  size_t c = b.rfind(" const");
  if (c != std::string::npos && c == b.size() - 6) b = b.substr(0, c);
  return b;
}

enum class ParamDir {
  InScalar, InObject, InObjectArray, OutScalar, OutObject, OutStruct,
  InBoundedString,
};

/* Set once per File by generateHeader/generateImpl; single-threaded
 * generation, one File processed at a time. */
const std::map<std::string, int> *g_charArrayTypedefs = nullptr;

ParamDir
paramDir(const Param &p)
{
  int stars = starCount(p.type);
  bool isConst = p.type.compare(0, 6, "const ") == 0;
  if (p.arrayCount > 0) return ParamDir::InObjectArray;
  if (stars == 0 && g_charArrayTypedefs) {
    std::string base = isConst ? p.type.substr(6) : p.type;
    auto it = g_charArrayTypedefs->find(base);
    if (it != g_charArrayTypedefs->end()) return ParamDir::InBoundedString;
  }
  if (stars == 0) return ParamDir::InScalar;
  if (stars >= 2) return ParamDir::OutObject;
  /* single star */
  if (isScalarType(p.type) && !isConst) return ParamDir::OutScalar;
  /* plain structs (leading underscore convention, e.g. _IOMDPrivateState)
   * are copied out by value in the reply */
  if (!baseType(p.type).empty() && baseType(p.type)[0] == '_' && !isConst)
    return ParamDir::OutStruct;
  return ParamDir::InObject; /* OSObject-derived by reference */
}

int
boundedStringSize(const Param &p)
{
  bool isConst = p.type.compare(0, 6, "const ") == 0;
  std::string base = isConst ? p.type.substr(6) : p.type;
  auto it = g_charArrayTypedefs->find(base);
  return it != g_charArrayTypedefs->end() ? it->second : 0;
}

struct MethodInfo {
  const Method *m;
  std::string cls;                 /* owning class name */
  std::vector<const Param *> inObjs, inObjArrays, outObjs, inScalars,
      outScalars, outStructs, inBoundedStrings;
  bool oneway = false;             /* fire-and-forget: void logical return */

  MethodInfo(const Class &c, const Method &method) : m(&method), cls(c.name) {
    for (auto &p : method.params) {
      switch (paramDir(p)) {
      case ParamDir::InScalar: inScalars.push_back(&p); break;
      case ParamDir::InObject: inObjs.push_back(&p); break;
      case ParamDir::InObjectArray: inObjArrays.push_back(&p); break;
      case ParamDir::OutScalar: outScalars.push_back(&p); break;
      case ParamDir::OutObject: outObjs.push_back(&p); break;
      case ParamDir::OutStruct: outStructs.push_back(&p); break;
      case ParamDir::InBoundedString: inBoundedStrings.push_back(&p); break;
      }
    }
    oneway = (method.returnType == "void");
  }

  size_t msgDescriptors() const {
    size_t n = 1 + inObjs.size();
    for (auto *p : inObjArrays) n += p->arrayCount;
    return n;
  }

  std::string id() const { return cls + "_" + m->name + "_ID"; }
  std::string args() const { return cls + "_" + m->name + "_Args"; }
  std::string msg() const { return cls + "_" + m->name + "_Msg"; }
  std::string rpl() const { return cls + "_" + m->name + "_Rpl"; }
  uint64_t msgid() const {
    std::string sig = cls + "::" + m->name + "(";
    for (auto &p : m->params) sig += p.type + ",";
    sig += ")";
    return iigHash(sig);
  }
};

/* Is this an init/free-style local virtual override (goes to
 * X_VirtualMethods, no RPC)? */
bool
isPlainVirtualOverride(const Method &m)
{
  return m.isOverride && !m.isLocal && m.actionType.empty() &&
         m.returnType != "kern_return_t" && m.returnType != "IOReturn";
}

bool
wantsRpc(const Method &m)
{
  return !m.isLocalOnly && !isPlainVirtualOverride(m);
}

/* KERNEL + TARGET together mark a raw callback signature (e.g.
 * IOUserClient::KernelCompletion) invoked directly by hand-written kernel
 * code via SimpleMemberFunctionCast, matched purely for its TYPE() so a
 * CreateAction helper can reference its _ID -- no wrapper or Invoke is
 * generated for it. Contrast with KERNEL alone (OSObject::SetDispatchQueue),
 * which still gets a caller-side wrapper; only the Dispatch case is
 * suppressed for those (see isKernelOnly use at the _Dispatch site). */
bool
wantsWrapper(const Method &m)
{
  return wantsRpc(m) && !(m.isKernelOnly && m.targetParam());
}

std::string
hex(uint64_t v)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%016llxULL", (unsigned long long)v);
  return buf;
}

/* Parameter list text "type name" (no defaults). */
std::string
paramDecl(const Param &p)
{
  return p.type + " " + p.name;
}

/* Same, but with bounded-string array typedefs (IODispatchQueueName etc.)
 * decayed to "const char *" -- used for the actual method signature (both
 * declaration and wrapper definition); the _Args macro keeps the raw
 * typedef since it's only ever pasted into another declaration position
 * where the decay happens implicitly anyway. */
std::string
paramDeclDecayed(const Param &p)
{
  if (paramDir(p) == ParamDir::InBoundedString) {
    bool isConst = p.type.compare(0, 6, "const ") == 0;
    return std::string(isConst ? "const char * " : "char * ") + p.name;
  }
  return paramDecl(p);
}

void
rewriteIigIncludes(std::string &text)
{
  size_t pos = 0;
  while ((pos = text.find(".iig>", pos)) != std::string::npos) {
    text.replace(pos, 5, ".h>  /* .iig include */");
    pos += 4;
  }
}

std::string
guardName(const File &f)
{
  /* IOHIDDevice.iig -> _IIG_IOHIDDEVICE_H (Apple uses framework-based names;
   * unimportant, only needs uniqueness) -- but the .iig itself carries its own
   * include guard in the passthrough text, so no extra guard is emitted. */
  (void)f;
  return "";
}

} /* namespace */

/* ---------------- header ---------------- */

static void
emitClassHeader(const File &f, const Class &c, std::string &o)
{
  std::vector<MethodInfo> infos;
  for (auto &m : c.methods)
    infos.emplace_back(c, m);

  char line[256];
  snprintf(line, sizeof(line), "/* generated class %s %s:%d-%d */\n",
           c.name.c_str(), f.basename.c_str(), c.firstLine, c.lastLine);
  o += line;
  o += "\n";

  /* IDs */
  for (auto &mi : infos) {
    if (!wantsRpc(*mi.m) || mi.m->isOverride) continue;
    o += "#define " + mi.id() + "            " + hex(mi.msgid()) + "\n";
  }
  o += "\n";

  /* Args */
  for (auto &mi : infos) {
    if (!wantsRpc(*mi.m)) continue;
    o += "#define " + mi.args() + " \\\n";
    if (mi.m->params.empty()) {
      o += "\n\n";
      continue;
    }
    for (size_t i = 0; i < mi.m->params.size(); i++) {
      o += "        " + paramDecl(mi.m->params[i]);
      o += (i + 1 < mi.m->params.size()) ? ", \\\n" : "\n";
    }
    o += "\n";
  }

  /* Methods macro */
  o += "#define " + c.name + "_Methods \\\n\\\npublic:\\\n\\\n";
  o += "    virtual kern_return_t\\\n    Dispatch(const IORPC rpc) APPLE_KEXT_OVERRIDE;\\\n\\\n";
  o += "    static kern_return_t\\\n    _Dispatch(" + c.name + " * self, const IORPC rpc);\\\n\\\n";

  for (auto &mi : infos) {
    const Method &m = *mi.m;
    if (m.isLocalOnly) {
      /* no RPC: plain declaration, implemented by hand on each side */
      if (isPlainVirtualOverride(m)) continue;
      o += "    " + std::string(m.isStatic ? "static " : "") + m.returnType +
           "\\\n    " + m.name + "(\\\n";
      for (size_t i = 0; i < m.params.size(); i++) {
        o += "        " + paramDeclDecayed(m.params[i]);
        if (i + 1 < m.params.size()) o += ",\\\n";
      }
      o += ");\\\n\\\n";
      continue;
    }
    if (!wantsRpc(m) || m.isOverride) continue;
    /* wrapper declaration */
    if (wantsWrapper(m)) {
      if (m.isStatic) {
        o += "    static " + m.returnType + "\\\n    " + m.name + "(\\\n";
        for (size_t i = 0; i < m.params.size(); i++) {
          o += "        " + paramDeclDecayed(m.params[i]);
          o += (i + 1 < m.params.size()) ? ",\\\n" : ");\\\n";
        }
        if (m.params.empty()) o += ");\\\n";
        o += "\\\n";
      } else {
        o += "    " + (m.returnType == "void" && mi.oneway ? std::string("kern_return_t")
                                                            : m.returnType) +
             "\\\n    " + m.name + "(\\\n";
        if (mi.oneway) o += "        IORPC rpc,\\\n";
        for (auto &p : m.params)
          o += "        " + paramDeclDecayed(p) + ",\\\n";
        o += "        OSDispatchMethod supermethod = NULL);\\\n\\\n";
      }
    }
    /* CreateAction helper for TYPE methods -- needs only this method's _ID
     * (always emitted above), not its wrapper/Invoke */
    if (!m.actionType.empty()) {
      o += "    kern_return_t\\\n    CreateAction" + m.name +
           "(size_t referenceSize, OSAction ** action);\\\n\\\n";
    }
  }

  /* protected: LOCAL impl declarations */
  o += "\\\nprotected:\\\n    /* _Impl methods */\\\n\\\n";
  for (auto &mi : infos) {
    const Method &m = *mi.m;
    if (!wantsRpc(m) || !m.isLocal) continue;
    o += "    " + std::string(m.isStatic ? "static " : "") + m.returnType +
         "\\\n    " + m.name + "_Impl(" + mi.args() + ");\\\n\\\n";
  }

  /* public: Invoke declarations */
  o += "\\\npublic:\\\n    /* _Invoke methods */\\\n\\\n";
  for (auto &mi : infos) {
    const Method &m = *mi.m;
    if (!wantsWrapper(m) || m.isOverride) continue;
    std::string handlerArgs = m.isStatic ? mi.args()
        : ("OSMetaClassBase * target, " + mi.args());
    o += "    typedef " + m.returnType + " (*" + m.name + "_Handler)(" +
         handlerArgs + ");\\\n";
    if (m.isStatic) {
      o += "    static kern_return_t\\\n    " + m.name +
           "_Invoke(const IORPC rpc,\\\n        " + m.name + "_Handler func);\\\n\\\n";
    } else {
      o += "    static kern_return_t\\\n    " + m.name +
           "_Invoke(const IORPC rpc,\\\n        OSMetaClassBase * target,\\\n        " +
           m.name + "_Handler func);\\\n\\\n";
      if (mi.m->targetParam()) {
        o += "    static kern_return_t\\\n    " + m.name +
             "_Invoke(const IORPC rpc,\\\n        OSMetaClassBase * target,\\\n        " +
             m.name + "_Handler func,\\\n        const OSMetaClass * targetActionClass);\\\n\\\n";
      }
    }
  }
  o += "\n\n";

  /* KernelMethods macro */
  o += "#define " + c.name + "_KernelMethods \\\n\\\nprotected:\\\n    /* _Impl methods */\\\n\\\n";
  for (auto &mi : infos) {
    const Method &m = *mi.m;
    if (!wantsRpc(m) || m.isLocal || m.isOverride) continue;
    o += "    " + std::string(m.isStatic ? "static " : "") + m.returnType +
         "\\\n    " + m.name + "_Impl(" + mi.args() + ");\\\n\\\n";
  }
  o += "\n\n";

  /* VirtualMethods macro */
  o += "#define " + c.name + "_VirtualMethods \\\n\\\npublic:\\\n\\\n";
  for (auto &mi : infos) {
    const Method &m = *mi.m;
    if (!isPlainVirtualOverride(m)) continue;
    o += "    virtual " + m.returnType + "\\\n    " + m.name + "(\\\n";
    for (size_t i = 0; i < m.params.size(); i++) {
      o += "        " + paramDeclDecayed(m.params[i]);
      o += (i + 1 < m.params.size()) ? ",\\\n" : "";
    }
    o += ") APPLE_KEXT_OVERRIDE;\\\n\\\n";
  }
  o += "\n\n";

  /* user-side class definition: only relevant to dext builds; the kernel
   * class is hand-written for non-NATIVE classes, so guard everything. */
  if (!c.isNative) {
    o += "#if !KERNEL\n\n";
    o += "class " + c.name + "Interface : public OSInterface\n{\npublic:\n};\n\n";
    o += "struct " + c.name + "_IVars;\nstruct " + c.name + "_LocalIVars;\n\n";
    o += "class " + c.name + " : public " + c.superName + ", public " +
         c.name + "Interface\n{\npublic:\n";
    o += "    union\n    {\n        " + c.name + "_IVars * ivars;\n        " +
         c.name + "_LocalIVars * lvars;\n    };\n";
    o += "    using super = " + c.superName + ";\n\n";
    o += "    " + c.name + "_Methods\n    " + c.name + "_VirtualMethods\n};\n\n";
    o += "#endif /* !KERNEL */\n";
  }
  o += "\n";
}

void
generateHeader(const File &f, std::string &o)
{
  g_charArrayTypedefs = &f.charArrayTypedefs;
  o += "/* iig-lite generated from " + f.basename + " - kernel-side subset;"
       " msgids are NOT Apple-ABI */\n\n";
  for (auto &ch : f.chunks) {
    if (ch.kind == Chunk::Text) {
      std::string t = ch.text;
      rewriteIigIncludes(t);
      o += t;
      continue;
    }
    const Class &c = f.classes[ch.classIndex];
    char line[256];
    snprintf(line, sizeof(line), "/* source class %s %s:%d-%d */\n",
             c.name.c_str(), f.basename.c_str(), c.firstLine, c.lastLine);
    o += line;
    o += "\n#if __DOCUMENTATION__\n#define KERNEL IIG_KERNEL\n\n";
    o += ch.text;
    o += "\n\n#undef KERNEL\n#else /* __DOCUMENTATION__ */\n\n";
    if (c.extendsName.empty())
      emitClassHeader(f, c, o);
    /* EXTENDS classes were merged into their target's macros */
    o += "#endif /* !__DOCUMENTATION__ */\n\n";
  }
}

/* ---------------- impl (.iig.cpp) ---------------- */

static void
emitMsgStructs(const MethodInfo &mi, std::string &o)
{
  const Method &m = *mi.m;
  /* message (send) */
  o += "struct " + mi.msg() + "_Content\n{\n    IORPCMessage __hdr;\n    OSObjectRef  __object;\n";
  for (auto *p : mi.inObjs)
    o += "    OSObjectRef  " + p->name + ";\n";
  for (auto *p : mi.inObjArrays)
    o += "    OSObjectRef __" + p->name + "[" + std::to_string(p->arrayCount) + "];\n";
  for (auto *p : mi.inScalars)
    o += "    " + p->type + "  " + p->name + ";\n";
  for (auto *p : mi.inBoundedStrings) {
    int n = boundedStringSize(*p);
    o += "    const char *  " + p->name + ";\n"
         "#if !defined(__LP64__)\n"
         "    uint32_t __" + p->name + "Pad;\n"
         "#endif /* !defined(__LP64__) */\n"
         "    char __" + p->name + "[" + std::to_string(n) + "];\n";
  }
  o += "};\n#pragma pack(4)\nstruct " + mi.msg() + "\n{\n    IORPCMessageMach           mach;\n";
  o += "    mach_msg_port_descriptor_t __object__descriptor;\n";
  for (auto *p : mi.inObjs)
    o += "    mach_msg_port_descriptor_t " + p->name + "__descriptor;\n";
  for (auto *p : mi.inObjArrays)
    o += "    mach_msg_port_descriptor_t " + p->name + "__descriptor[" +
         std::to_string(p->arrayCount) + "];\n";
  o += "    " + mi.msg() + "_Content content;\n};\n#pragma pack()\n";
  o += "#define " + mi.msg() + "_ObjRefs (" +
       std::to_string(mi.msgDescriptors()) + ")\n\n";

  /* reply */
  o += "struct " + mi.rpl() + "_Content\n{\n    IORPCMessage __hdr;\n";
  for (auto *p : mi.outObjs)
    o += "    OSObjectRef  " + p->name + ";\n";
  for (auto *p : mi.outScalars)
    o += "    " + baseType(p->type) + "  " + p->name + ";\n";
  for (auto *p : mi.outStructs)
    o += "    " + baseType(p->type) + "  " + p->name + ";\n";
  o += "};\n#pragma pack(4)\nstruct " + mi.rpl() + "\n{\n    IORPCMessageMach           mach;\n";
  for (auto *p : mi.outObjs)
    o += "    mach_msg_port_descriptor_t " + p->name + "__descriptor;\n";
  o += "    " + mi.rpl() + "_Content content;\n};\n#pragma pack()\n";
  o += "#define " + mi.rpl() + "_ObjRefs (" + std::to_string(mi.outObjs.size()) + ")\n\n";

  /* invocation union */
  o += "typedef union\n{\n    const IORPC rpc;\n    struct\n    {\n";
  o += "        const struct " + mi.msg() + " * message;\n";
  o += "        struct " + mi.rpl() + "       * reply;\n";
  o += "        uint32_t sendSize;\n        uint32_t replySize;\n    };\n}\n" +
       mi.cls + "_" + m.name + "_Invocation;\n";
}

static void
emitWrapper(const Class &c, const MethodInfo &mi, std::string &o)
{
  const Method &m = *mi.m;
  const Param *target = m.targetParam();

  o += (m.returnType == "void" && mi.oneway) ? "kern_return_t" : m.returnType;
  o += "\n" + c.name + "::" + m.name + "(\n";
  if (mi.oneway) o += "        IORPC rpc,\n";
  for (size_t i = 0; i < m.params.size(); i++) {
    o += "        " + paramDeclDecayed(m.params[i]);
    bool more = (i + 1 < m.params.size()) || !m.isStatic;
    o += more ? ",\n" : ")\n";
  }
  if (!m.isStatic) o += "        OSDispatchMethod supermethod)\n";
  else if (m.params.empty()) o += ")\n";
  o += "{\n    kern_return_t ret;\n";

  if (mi.oneway) {
    /* callback-style: serialize into the caller-provided reply buffer */
    o += "    struct " + mi.msg() + " * msg = (typeof(msg)) rpc.reply;\n\n";
  } else {
    o += "    union\n    {\n        " + mi.msg() + " msg;\n"
         "        struct\n        {\n            " + mi.rpl() + " rpl;\n"
         "            mach_msg_max_trailer_t trailer;\n        } rpl;\n    } buf;\n";
    o += "    struct " + mi.msg() + " * msg = &buf.msg;\n";
    o += "    struct " + mi.rpl() + " * rpl = &buf.rpl.rpl;\n\n";
  }

  o += "    memset(msg, 0, sizeof(struct " + mi.msg() + "));\n";
  o += "    msg->mach.msgh.msgh_id   = kIORPCVersion190615;\n";
  o += "    msg->mach.msgh.msgh_size = sizeof(*msg);\n";
  bool simpleReply = mi.outObjs.empty();
  o += "    msg->content.__hdr.flags = " + std::string(mi.oneway ? "1" : "0") +
       "*kIORPCMessageOneway\n"
       "                             | " + std::string(simpleReply ? "1" : "0") +
       "*kIORPCMessageSimpleReply\n"
       "                             | 0*kIORPCMessageLocalHost\n"
       "                             | " + std::string(mi.oneway ? "1" : "0") +
       "*kIORPCMessageOnqueue;\n";
  o += "    msg->content.__hdr.msgid = " + mi.id() + ";\n";
  if (m.isStatic)
    o += "    msg->content.__object = (OSObjectRef) OSTypeID(" + c.name + ");\n";
  else if (mi.oneway && target)
    o += "    msg->content.__object = (OSObjectRef) " + target->name + ";\n";
  else
    o += "    msg->content.__object = (OSObjectRef) this;\n";
  o += "    msg->content.__hdr.objectRefs = " + mi.msg() + "_ObjRefs;\n";
  o += "    msg->mach.msgh_body.msgh_descriptor_count = " +
       std::to_string(mi.msgDescriptors()) + ";\n\n";
  o += "    msg->__object__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;\n\n";
  for (auto *p : mi.inObjs) {
    o += "    msg->" + p->name + "__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;\n";
    o += "    msg->content." + p->name + " = (OSObjectRef) " + p->name + ";\n\n";
  }
  for (auto *p : mi.inObjArrays) {
    o += "    for (unsigned int __i = 0; __i < " + std::to_string(p->arrayCount) +
         "; __i++)\n    {\n";
    o += "        msg->" + p->name + "__descriptor[__i].type = MACH_MSG_PORT_DESCRIPTOR;\n";
    o += "        msg->content.__" + p->name + "[__i] = (OSObjectRef) " +
         p->name + "[__i];\n    }\n\n";
  }
  for (auto *p : mi.inScalars)
    o += "    msg->content." + p->name + " = " + p->name + ";\n\n";
  for (auto *p : mi.inBoundedStrings) {
    o += "    msg->content." + p->name + " = NULL;\n\n";
    o += "    strlcpy(&msg->content.__" + p->name + "[0], " + p->name +
         ", sizeof(msg->content.__" + p->name + "));\n\n";
  }

  if (mi.oneway) {
    o += "\n    ret = kIOReturnSuccess;\n\n    return (ret);\n}\n\n";
    return;
  }

  o += "    IORPC _rpc = { .message = &buf.msg.mach, .reply = &buf.rpl.rpl.mach,"
       " .sendSize = sizeof(buf.msg), .replySize = sizeof(buf.rpl) };\n";
  if (m.isStatic) {
    o += "    ret = OSMTypeID(" + c.name + ")->Invoke(_rpc);\n\n";
  } else {
    o += "    if (supermethod) ret = supermethod((OSObject *)this, _rpc);\n";
    o += "    else             ret = ((OSObject *)this)->Invoke(_rpc);\n\n";
  }
  o += "    if (kIOReturnSuccess == ret)\n    do {\n        {\n";
  o += "            if (rpl->mach.msgh.msgh_size                  != sizeof(*rpl)) { ret = kIOReturnIPCError; break; };\n";
  o += "            if (rpl->content.__hdr.msgid                  != " + mi.id() +
       ") { ret = kIOReturnIPCError; break; };\n";
  o += "            if (rpl->mach.msgh_body.msgh_descriptor_count != " +
       std::to_string(mi.outObjs.size()) + ") { ret = kIOReturnIPCError; break; };\n";
  o += "            if (" + mi.rpl() + "_ObjRefs   != rpl->content.__hdr.objectRefs)"
       " { ret = kIOReturnIPCError; break; };\n";
  o += "        }\n    }\n    while (false);\n";
  o += "    if (kIOReturnSuccess == ret)\n    {\n";
  for (auto *p : mi.outObjs) {
    std::string bt = baseType(p->type);
    o += "        *" + p->name + " = OSDynamicCast(" + bt +
         ", (OSObject *) rpl->content." + p->name + ");\n";
    o += "        if (rpl->content." + p->name + " && !*" + p->name +
         ") ret = kIOReturnBadArgument;\n";
  }
  for (auto *p : mi.outScalars)
    o += "        if (" + p->name + ") *" + p->name + " = rpl->content." +
         p->name + ";\n";
  for (auto *p : mi.outStructs)
    o += "        if (" + p->name + ") *" + p->name + " = rpl->content." +
         p->name + ";\n";
  o += "    }\n\n";
  o += (m.returnType == "void") ? "    (void) ret;\n}\n\n" : "    return (ret);\n}\n\n";
}

static void
emitInvoke(const Class &c, const MethodInfo &mi, std::string &o)
{
  const Method &m = *mi.m;
  const Param *target = m.targetParam();

  auto emitOne = [&](bool withActionClass) {
    o += "kern_return_t\n" + c.name + "::" + m.name + "_Invoke(const IORPC _rpc";
    if (!m.isStatic) o += ",\n        OSMetaClassBase * target";
    o += ",\n        " + m.name + "_Handler func";
    if (withActionClass) o += ",\n        const OSMetaClass * targetActionClass";
    o += ")\n{\n";
    o += "    " + c.name + "_" + m.name + "_Invocation rpc = { _rpc };\n";
    if (!mi.oneway) o += "    kern_return_t ret;\n";
    for (auto *p : mi.inObjs)
      o += "    " + baseType(p->type) + " * " + p->name + ";\n";
    for (auto *p : mi.inObjArrays)
      o += "    " + baseType(p->type) + " * " + p->name + "[" +
           std::to_string(p->arrayCount) + "];\n";
    o += "\n    if (" + mi.msg() + "_ObjRefs != rpc.message->content.__hdr.objectRefs)"
         " return (kIOReturnIPCError);\n";
    for (auto *p : mi.inObjArrays) {
      std::string bt = baseType(p->type);
      o += "    for (unsigned int __i = 0; __i < " + std::to_string(p->arrayCount) +
           "; __i++)\n    {\n";
      o += "        " + p->name + "[__i] = OSDynamicCast(" + bt +
           ", (OSObject *) rpc.message->content.__" + p->name + "[__i]);\n";
      o += "        if (!" + p->name + "[__i] && rpc.message->content.__" +
           p->name + "[__i]) return (kIOReturnBadArgument);\n    }\n";
    }
    for (auto *p : mi.inObjs) {
      std::string bt = baseType(p->type);
      if (withActionClass && target == p) {
        o += "    if (targetActionClass) {\n";
        o += "        " + p->name + " = (" + bt + " *) OSMetaClassBase::safeMetaCast("
             "(OSObject *) rpc.message->content." + p->name + ", targetActionClass);\n";
        o += "    } else {\n";
        o += "        " + p->name + " = OSDynamicCast(" + bt +
             ", (OSObject *) rpc.message->content." + p->name + ");\n    }\n";
      } else {
        o += "    " + p->name + " = OSDynamicCast(" + bt +
             ", (OSObject *) rpc.message->content." + p->name + ");\n";
      }
      o += "    if (!" + p->name + " && rpc.message->content." + p->name +
           ") return (kIOReturnBadArgument);\n";
    }
    for (auto *p : mi.inBoundedStrings) {
      o += "    if (strnlen(&rpc.message->content.__" + p->name + "[0], "
           "sizeof(rpc.message->content.__" + p->name + ")) >= "
           "sizeof(rpc.message->content.__" + p->name +
           ")) return kIOReturnBadArgument;\n";
    }

    bool funcReturnsVoid = !mi.oneway && m.returnType == "void";
    o += "\n    " + std::string((mi.oneway || funcReturnsVoid) ? "" : "ret = ") + "(*func)(";
    if (!m.isStatic) o += "target,\n        ";
    for (size_t i = 0; i < m.params.size(); i++) {
      const Param &p = m.params[i];
      switch (paramDir(p)) {
      case ParamDir::InScalar:
        o += "rpc.message->content." + p.name;
        break;
      case ParamDir::InObject:
      case ParamDir::InObjectArray:
        o += p.name;
        break;
      case ParamDir::OutScalar:
      case ParamDir::OutStruct:
        o += "&rpc.reply->content." + p.name;
        break;
      case ParamDir::OutObject:
        o += "(" + p.type + ")&rpc.reply->content." + p.name;
        break;
      case ParamDir::InBoundedString:
        o += "&rpc.message->content.__" + p.name + "[0]";
        break;
      }
      if (i + 1 < m.params.size()) o += ",\n        ";
    }
    o += ");\n\n";

    if (mi.oneway) {
      o += "    return (kIOReturnSuccess);\n}\n\n";
      return;
    }
    if (funcReturnsVoid) o += "    ret = kIOReturnSuccess;\n\n";
    o += "    if (kIOReturnSuccess != ret) return (ret);\n\n";
    o += "    rpc.reply->content.__hdr.msgid = " + mi.id() + ";\n";
    o += "    rpc.reply->content.__hdr.flags = kIORPCMessageOneway;\n";
    o += "    rpc.reply->mach.msgh.msgh_id   = kIORPCVersion190615Reply;\n";
    o += "    rpc.reply->mach.msgh.msgh_size = sizeof(*rpc.reply);\n";
    o += "    rpc.reply->mach.msgh_body.msgh_descriptor_count = " +
         std::to_string(mi.outObjs.size()) + ";\n";
    o += "    rpc.reply->content.__hdr.objectRefs = " + mi.rpl() + "_ObjRefs;\n";
    for (auto *p : mi.outObjs)
      o += "    rpc.reply->" + p->name + "__descriptor.type = MACH_MSG_PORT_DESCRIPTOR;\n";
    o += "\n    return (ret);\n}\n\n";
  };

  if (target && !m.isStatic) {
    /* corpus emits the actionClass-less overload forwarding to the full one */
    o += "kern_return_t\n" + c.name + "::" + m.name + "_Invoke(const IORPC _rpc,\n"
         "        OSMetaClassBase * target,\n        " + m.name + "_Handler func)\n{\n"
         "    return " + c.name + "::" + m.name + "_Invoke(_rpc, target, func, NULL);\n}\n\n";
    emitOne(true);
  } else {
    emitOne(false);
  }
}

static void
emitClassImpl(const File &f, const Class &c, std::string &o)
{
  std::vector<MethodInfo> infos;
  for (auto &m : c.methods)
    infos.emplace_back(c, m);

  /* Msg/Rpl structs */
  for (auto &mi : infos) {
    if (!wantsRpc(*mi.m) || mi.m->isOverride) continue;
    emitMsgStructs(mi, o);
  }

  /* Dispatch: skipped entirely for root classes with no superclass
   * (OSMetaClassBase) -- iig only generates RPC scaffolding for classes that
   * actually override Dispatch, and a class with no super has nothing valid
   * to fall back to. */
  if (c.superName.empty()) return;

  o += "kern_return_t\n" + c.name + "::Dispatch(const IORPC rpc)\n{\n"
       "    return _Dispatch(this, rpc);\n}\n\n";
  o += "kern_return_t\n" + c.name + "::_Dispatch(" + c.name +
       " * self, const IORPC rpc)\n{\n"
       "    kern_return_t ret = kIOReturnUnsupported;\n"
       "    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);\n\n"
       "    switch (msg->msgid)\n    {\n";
  for (auto &mi : infos) {
    const Method &m = *mi.m;
    if (!wantsRpc(m) || m.isOverride) continue;
    /* statics dispatch via MetaClass; KERNEL-only methods are always
     * kernel-initiated (kernel never receives them). LOCAL only affects
     * where _Impl is declared (KernelMethods vs generic Methods), not
     * whether a Dispatch case exists -- LOCAL methods DO get dispatched
     * (e.g. OSAction::Aborted, IOService::Start/Stop). */
    if (m.isStatic || m.isKernelOnly) continue;
    o += "#if KERNEL\n";
    o += "        case " + mi.id() + ":\n        {\n";
    o += "            ret = " + c.name + "::" + m.name + "_Invoke(rpc, self, "
         "SimpleMemberFunctionCast(" + c.name + "::" + m.name + "_Handler, *self, &" +
         c.name + "::" + m.name + "_Impl));\n            break;\n        }\n";
    o += "#endif /* !KERNEL */\n";
  }
  o += "\n        default:\n";
  /* OSMetaClassBase is the hierarchy root: hand-written elsewhere in the
   * kernel (see IOUserServer.cpp's OSMetaClassBase::Invoke/Dispatch), not
   * iig-generated -- it has no _Dispatch, only the instance Dispatch. */
  if (c.superName == "OSMetaClassBase")
    o += "            ret = self->OSMetaClassBase::Dispatch(rpc);\n";
  else
    o += "            ret = " + c.superName + "::_Dispatch(self, rpc);\n";
  o += "            break;\n    }\n\n    return (ret);\n}\n\n";

  /* MetaClass::Dispatch (kernel side only; the !KERNEL metaclass is not
   * generated by iig-lite) */
  o += "#if KERNEL\nkern_return_t\n" + c.name +
       "::MetaClass::Dispatch(const IORPC rpc)\n{\n"
       "    kern_return_t ret = kIOReturnUnsupported;\n"
       "    IORPCMessage * msg = IORPCMessageFromMach(rpc.message, false);\n\n"
       "    switch (msg->msgid)\n    {\n";
  for (auto &mi : infos) {
    const Method &m = *mi.m;
    if (!wantsRpc(m) || m.isOverride) continue;
    if (!m.isStatic || m.isLocal) continue;
    o += "        case " + mi.id() + ":\n";
    o += "            ret = " + c.name + "::" + m.name + "_Invoke(rpc, &" +
         c.name + "::" + m.name + "_Impl);\n            break;\n";
  }
  o += "\n        default:\n            ret = OSMetaClassBase::Dispatch(rpc);\n"
       "            break;\n    }\n\n    return (ret);\n}\n#endif /* KERNEL */\n\n";

  /* wrappers, CreateAction helpers, Invokes */
  for (auto &mi : infos) {
    const Method &m = *mi.m;
    if (!wantsRpc(m) || m.isOverride) continue;
    bool wrapper = wantsWrapper(m);
    if (wrapper) emitWrapper(c, mi, o);
    if (!m.actionType.empty()) {
      /* TYPE(Cls::Method) -- action creator */
      std::string t = m.actionType; /* "IOHIDDevice::CompleteReport" */
      size_t sep = t.find("::");
      std::string typeId = (sep == std::string::npos)
          ? c.name + "_" + t + "_ID"
          : t.substr(0, sep) + "_" + t.substr(sep + 2) + "_ID";
      o += "kern_return_t\n" + c.name + "::CreateAction" + m.name +
           "(size_t referenceSize, OSAction ** action)\n{\n"
           "    return OSAction::Create(this, " + mi.id() + ", " + typeId +
           ", referenceSize, action);\n}\n\n";
    }
    if (wrapper) emitInvoke(c, mi, o);
  }
}

void
generateImpl(const File &f, std::string &o)
{
  g_charArrayTypedefs = &f.charArrayTypedefs;
  std::string stem = f.basename;
  size_t dot = stem.rfind('.');
  if (dot != std::string::npos) stem = stem.substr(0, dot);

  o += "/* iig-lite generated from " + f.basename + " - kernel-side subset;"
       " msgids are NOT Apple-ABI */\n\n";
  o += "#undef IIG_IMPLEMENTATION\n#define IIG_IMPLEMENTATION \t" + f.basename + "\n\n";
  o += "#if KERNEL\n#include <libkern/c++/OSString.h>\n#else\n"
       "#include <DriverKit/DriverKit.h>\n#endif /* KERNEL */\n"
       "#include <DriverKit/IOReturn.h>\n";
  if (!f.frameworkName.empty())
    o += "#include <" + f.frameworkName + "/" + stem + ".h>\n\n";
  else
    o += "#include \"" + stem + ".h\"\n\n";
  if (!f.implText.empty()) {
    o += f.implText;
    o += "\n";
  }
  o += "#if __has_builtin(__builtin_load_member_function_pointer)\n"
       "#define SimpleMemberFunctionCast(cfnty, self, func) "
       "(cfnty)__builtin_load_member_function_pointer(self, func)\n#else\n"
       "#define SimpleMemberFunctionCast(cfnty, self, func) "
       "({ union { typeof(func) memfun; cfnty cfun; } pair; pair.memfun = func; pair.cfun; })\n"
       "#endif\n\n";

  for (auto &c : f.classes) {
    if (!c.extendsName.empty()) continue; /* merged into target */
    emitClassImpl(f, c, o);
  }
}

} /* namespace iig */
