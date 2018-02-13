#include <algorithm>
#include <functional>
#include <memory>
#include <map>
#include <cassert>
#include <vector>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include "llvm/ADT/Optional.h"
#include "llvm/Support/Casting.h"
#include "LibXMLUtil.h"
#include "StringTree.h"
#include "XMLString.h"
#include "XMLWalker.h"
#include "AttrProc.h"
#include "XcodeMlNns.h"
#include "XcodeMlName.h"
#include "XcodeMlType.h"
#include "XcodeMlEnvironment.h"
#include "SourceInfo.h"
#include "TypeAnalyzer.h"
#include "NnsAnalyzer.h"
#include "CodeBuilder.h"
#include "ClangClassHandler.h"
#include "XcodeMlUtil.h"

namespace cxxgen = CXXCodeGen;

#define CCH_ARGS                                                              \
  xmlNodePtr node __attribute__((unused)),                                    \
      const CodeBuilder &w __attribute__((unused)),                           \
      SourceInfo &src __attribute__((unused))

#define DEFINE_CCH(name) static XcodeMl::CodeFragment name(CCH_ARGS)

using cxxgen::makeInnerNode;
using cxxgen::makeTokenNode;
using XcodeMl::CodeFragment;

DEFINE_CCH(callCodeBuilder) {
  return makeInnerNode(ProgramBuilder.walkChildren(node, src));
}

DEFINE_CCH(BreakStmtProc) {
  return makeTokenNode("break");
}

DEFINE_CCH(callExprProc) {
  return (w["functionCall"])(w, node, src);
}

DEFINE_CCH(CompoundStmtProc) {
  const auto stmtNodes = findNodes(node, "clangStmt", src.ctxt);
  std::vector<CXXCodeGen::StringTreeRef> stmts;
  for (auto &&stmtNode : stmtNodes) {
    stmts.push_back(w.walk(stmtNode, src) + makeTokenNode(";"));
  }
  return insertNewLines(stmts);
}

DEFINE_CCH(CXXCtorExprProc) {
  return makeTokenNode("(") + cxxgen::join(", ", w.walkChildren(node, src))
      + makeTokenNode(")");
}

DEFINE_CCH(CXXDeleteExprProc) {
  const auto allocated = findFirst(node, "*", src.ctxt);
  return makeTokenNode("delete") + w.walk(allocated, src);
}

DEFINE_CCH(DeclRefExprProc) {
  const auto nameNode = findFirst(node, "name", src.ctxt);
  const auto name = getQualifiedNameFromNameNode(nameNode, src);

  if (const auto TAL = findFirst(node, "TemplateArgumentLoc", src.ctxt)) {
    const auto templArgNodes = findNodes(TAL, "*", src.ctxt);
    std::vector<CodeFragment> args;
    for (auto &&argNode : templArgNodes) {
      args.push_back(w.walk(argNode, src));
    }
    return name.toString(src.typeTable, src.nnsTable) + makeTokenNode("<")
        + join(",", args) + makeTokenNode(">");
  }

  return name.toString(src.typeTable, src.nnsTable);
}

DEFINE_CCH(DeclStmtProc) {
  const auto declNode = findFirst(node, "clangDecl", src.ctxt);
  return w.walk(declNode, src);
}

DEFINE_CCH(emitTokenAttrValue) {
  const auto token = getProp(node, "token");
  return makeTokenNode(token);
}

DEFINE_CCH(FunctionTemplateProc) {
  if (const auto typeTableNode =
          findFirst(node, "xcodemlTypeTable", src.ctxt)) {
    src.typeTable = expandEnvironment(src.typeTable, typeTableNode, src.ctxt);
  }
  if (const auto nnsTableNode = findFirst(node, "xcodemlNnsTable", src.ctxt)) {
    src.nnsTable = expandNnsMap(src.nnsTable, nnsTableNode, src.ctxt);
  }
  const auto paramNodes =
      findNodes(node, "clangDecl[@class='TemplateTypeParm']", src.ctxt);
  const auto body = findFirst(node, "functionDefinition", src.ctxt);

  std::vector<CXXCodeGen::StringTreeRef> params;
  for (auto &&paramNode : paramNodes) {
    params.push_back(w.walk(paramNode, src));
  }

  return makeTokenNode("template") + makeTokenNode("<") + join(",", params)
      + makeTokenNode(">") + w.walk(body, src);
}

DEFINE_CCH(TemplateTypeParmProc) {
  const auto nameNode = findFirst(node, "name", src.ctxt);
  const auto name = getQualifiedNameFromNameNode(nameNode, src);
  const auto nameSpelling = name.toString(src.typeTable, src.nnsTable);

  const auto dtident = getProp(node, "type");
  auto T = src.typeTable.at(dtident);
  auto TTPT = llvm::cast<XcodeMl::TemplateTypeParm>(T.get());
  assert(TTPT);
  TTPT->setSpelling(nameSpelling);

  return makeTokenNode("typename") + nameSpelling;
}

XcodeMl::CodeFragment
makeBases(const XcodeMl::ClassType &T, SourceInfo &src) {
  using namespace XcodeMl;
  const auto bases = T.getBases();
  std::vector<CodeFragment> decls;
  std::transform(bases.begin(),
      bases.end(),
      std::back_inserter(decls),
      [&src](ClassType::BaseClass base) {
        const auto T = src.typeTable.at(std::get<1>(base));
        const auto classT = llvm::cast<ClassType>(T.get());
        assert(classT);
        assert(classT->name().hasValue());
        return makeTokenNode(std::get<0>(base))
            + makeTokenNode(std::get<2>(base) ? "virtual" : "")
            + *(classT->name());
      });
  return decls.empty() ? cxxgen::makeVoidNode()
                       : makeTokenNode(":") + cxxgen::join(",", decls);
}

CodeFragment
emitClassDefinition(xmlNodePtr node,
    const CodeBuilder &w,
    SourceInfo &src,
    const XcodeMl::ClassType &classType) {
  if (isTrueProp(node, "is_implicit", false)) {
    return cxxgen::makeVoidNode();
  }

  std::vector<XcodeMl::CodeFragment> decls;

  for (xmlNodePtr memberNode = xmlFirstElementChild(node); memberNode;
       memberNode = xmlNextElementSibling(memberNode)) {
    /* Traverse `memberNode` regardless of whether `CodeBuilder` prints it. */
    const auto decl = w.walk(memberNode, src);

    const auto accessProp = getPropOrNull(memberNode, "access");
    if (accessProp.hasValue()) {
      const auto access = *accessProp;
      decls.push_back(makeTokenNode(access) + makeTokenNode(":") + decl);
    } else {
      decls.push_back(makeTokenNode(
          "\n/* Ignored a member with no access specifier */\n"));
    }
  }

  const auto classKey = makeTokenNode(getClassKey(classType.classKind()));
  const auto name = classType.name().getValueOr(cxxgen::makeVoidNode());

  return classKey + name + makeBases(classType, src) + makeTokenNode("{")
      + separateByBlankLines(decls) + makeTokenNode("}") + makeTokenNode(";")
      + cxxgen::makeNewLineNode();
}

void
setClassName(XcodeMl::ClassType &classType, xmlNodePtr node, SourceInfo &src) {
  const auto nameNode = findFirst(node, "name", src.ctxt);
  if (!nameNode || isEmpty(nameNode)) {
    /* `classType` is unnamed.
     * Unnamed classes are problematic, so give a name to `classType`
     * such as `__xcodeml_1`.
     */
    classType.setName(src.getUniqueName());
    return;
  }
  const auto className = getUnqualIdFromNameNode(nameNode);
  const auto nameSpelling = className->toString(src.typeTable);
  classType.setName(nameSpelling);
}

DEFINE_CCH(CXXRecordProc) {
  if (isTrueProp(node, "is_implicit", false)) {
    return cxxgen::makeVoidNode();
  }

  const auto T = src.typeTable.at(getProp(node, "type"));
  auto classT = llvm::dyn_cast<XcodeMl::ClassType>(T.get());
  assert(classT);

  setClassName(*classT, node, src);
  const auto nameSpelling = *(classT->name()); // now class name must exist

  if (isTrueProp(node, "is_this_declaration_a_definition", false)) {
    return emitClassDefinition(node, ClassDefinitionBuilder, src, *classT);
  }

  /* forward declaration */
  const auto classKey = getClassKey(classT->classKind());
  return makeTokenNode(classKey) + nameSpelling + makeTokenNode(";");
}

DEFINE_CCH(CXXTemporaryObjectExprProc) {
  const auto resultT = src.typeTable.at(getProp(node, "type"));
  const auto name = llvm::cast<XcodeMl::ClassType>(resultT.get())->name();
  assert(name.hasValue());
  auto children = findNodes(node, "*[position() > 1]", src.ctxt);
  // ignore first child, which represents the result (class) type of
  // the clang::CXXTemporaryObjectExpr
  std::vector<CodeFragment> args;
  for (auto child : children) {
    args.push_back(w.walk(child, src));
  }
  return *name + makeTokenNode("(") + join(",", args) + makeTokenNode(")");
}

const ClangClassHandler ClangStmtHandler("class",
    cxxgen::makeInnerNode,
    callCodeBuilder,
    {
        std::make_tuple("BreakStmt", BreakStmtProc),
        std::make_tuple("CallExpr", callExprProc),
        std::make_tuple("CharacterLiteral", emitTokenAttrValue),
        std::make_tuple("CompoundStmt", CompoundStmtProc),
        std::make_tuple("CXXConstructExpr", CXXCtorExprProc),
        std::make_tuple("CXXDeleteExpr", CXXDeleteExprProc),
        std::make_tuple("CXXTemporaryObjectExpr", CXXTemporaryObjectExprProc),
        std::make_tuple("DeclStmt", DeclStmtProc),
        std::make_tuple("DeclRefExpr", DeclRefExprProc),
        std::make_tuple("FloatingLiteral", emitTokenAttrValue),
        std::make_tuple("IntegerLiteral", emitTokenAttrValue),
    });

DEFINE_CCH(FriendDeclProc) {
  if (auto TL = findFirst(node, "clangTypeLoc", src.ctxt)) {
    /* friend class declaration */
    const auto dtident = getProp(TL, "type");
    const auto T = src.typeTable.at(dtident);
    return makeTokenNode("friend")
        + makeDecl(T, cxxgen::makeVoidNode(), src.typeTable)
        + makeTokenNode(";");
  }
  return makeTokenNode("friend") + callCodeBuilder(node, w, src);
}

DEFINE_CCH(FunctionProc) {
  const auto type = getProp(node, "xcodemlType");
  const auto paramNames = getParamNames(node, src);
  auto acc = makeFunctionDeclHead(node, paramNames, src, true);

  if (const auto ctorInitList =
          findFirst(node, "constructorInitializerList", src.ctxt)) {
    acc = acc + w.walk(ctorInitList, src);
  }

  if (const auto bodyNode = findFirst(node, "clangStmt", src.ctxt)) {
    const auto body = w.walk(bodyNode, src);
    acc = acc + wrapWithBrace(body);
  } else {
    acc = acc + makeTokenNode(";");
  }

  return wrapWithLangLink(acc, node, src);
}

DEFINE_CCH(TranslationUnitProc) {
  if (const auto typeTableNode =
          findFirst(node, "xcodemlTypeTable", src.ctxt)) {
    src.typeTable = expandEnvironment(src.typeTable, typeTableNode, src.ctxt);
  }
  if (const auto nnsTableNode = findFirst(node, "xcodemlNnsTable", src.ctxt)) {
    src.nnsTable = expandNnsMap(src.nnsTable, nnsTableNode, src.ctxt);
  }
  const auto declNodes = findNodes(node, "clangDecl", src.ctxt);
  std::vector<CXXCodeGen::StringTreeRef> decls;
  for (auto &&declNode : declNodes) {
    decls.push_back(w.walk(declNode, src));
  }
  return separateByBlankLines(decls);
}

DEFINE_CCH(TypedefProc) {
  if (isTrueProp(node, "is_implicit", 0)) {
    return cxxgen::makeVoidNode();
  }
  const auto dtident = getProp(node, "xcodemlTypedefType");
  const auto T = src.typeTable.at(dtident);

  const auto nameNode = findFirst(node, "name", src.ctxt);
  const auto typedefName =
      getUnqualIdFromNameNode(nameNode)->toString(src.typeTable);

  return makeTokenNode("typedef") + makeDecl(T, typedefName, src.typeTable)
      + makeTokenNode(";");
}

const ClangClassHandler ClangDeclHandler("class",
    cxxgen::makeInnerNode,
    callCodeBuilder,
    {
        std::make_tuple("CXXRecord", CXXRecordProc),
        std::make_tuple("Friend", FriendDeclProc),
        std::make_tuple("Function", FunctionProc),
        std::make_tuple("FunctionTemplate", FunctionTemplateProc),
        std::make_tuple("TemplateTypeParm", TemplateTypeParmProc),
        std::make_tuple("TranslationUnit", TranslationUnitProc),
        std::make_tuple("Typedef", TypedefProc),
    });

DEFINE_CCH(BuiltinTypeProc) {
  const auto dtident = getProp(node, "type");
  return makeDecl(
      src.typeTable.at(dtident), cxxgen::makeVoidNode(), src.typeTable);
}

const ClangClassHandler ClangTypeLocHandler("class",
    cxxgen::makeInnerNode,
    callCodeBuilder,
    {
        std::make_tuple("Builtin", BuiltinTypeProc),
    });
