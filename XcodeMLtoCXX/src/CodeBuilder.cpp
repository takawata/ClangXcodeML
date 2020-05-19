#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <memory>
#include <map>
#include <cassert>
#include <vector>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include "llvm/ADT/Optional.h"
#include "llvm/Support/Casting.h"
#include "XMLString.h"
#include "XMLWalker.h"
#include "AttrProc.h"
#include "Stream.h"
#include "StringTree.h"
#include "Util.h"
#include "XcodeMlNns.h"
#include "XcodeMlName.h"
#include "XcodeMlOperator.h"
#include "XcodeMlType.h"
#include "XcodeMlTypeTable.h"
#include "XcodeMlUtil.h"
#include "NnsAnalyzer.h"
#include "TypeAnalyzer.h"
#include "SourceInfo.h"
#include "CodeBuilder.h"
#include "ClangDeclHandler.h"
#include "ClangNestedNameSpecHandler.h"
#include "ClangStmtHandler.h"
#include "ClangTypeLocHandler.h"
#include "LibXMLUtil.h"

namespace cxxgen = CXXCodeGen;

using cxxgen::StringTreeRef;
using cxxgen::makeTokenNode;
using cxxgen::makeInnerNode;
using cxxgen::makeNewLineNode;
using cxxgen::makeVoidNode;

using cxxgen::insertNewLines;
using cxxgen::separateByBlankLines;
using XcodeMl::makeOpNode;

namespace {

XcodeMl::CodeFragment
makeNestedNameSpec(const XcodeMl::NnsRef &nns, const SourceInfo &src) {
  return nns->makeDeclaration(src.typeTable, src.nnsTable);
}

XcodeMl::CodeFragment
makeNestedNameSpec(const std::string &ident, const SourceInfo &src) {
  const auto nns = getOrNull(src.nnsTable, ident);
  if (!nns.hasValue()) {
    std::cerr << "In makeNestedNameSpec:" << std::endl
              << "Undefined NNS: '" << ident << "'" << std::endl;
    std::abort();
  }
  return makeNestedNameSpec(*nns, src);
}

std::vector<XcodeMl::CodeFragment>
createNodes(xmlNodePtr node,
    const char *xpath,
    const CodeBuilder &w,
    SourceInfo &src) {
  std::vector<XcodeMl::CodeFragment> vec;
  const auto targetNodes = findNodes(node, xpath, src.ctxt);
  for (auto &&targetNode : targetNodes) {
    vec.push_back(w.walk(targetNode, src));
  }
  return vec;
}

/*!
 * \brief Arguments to be passed to CodeBuilder::Procedure.
 */
#define CB_ARGS                                                               \
  const CodeBuilder &w __attribute__((unused)),                               \
      xmlNodePtr node __attribute__((unused)),                                \
      SourceInfo &src __attribute__((unused))

/*!
 * \brief Define new CodeBuilder::Procedure named \c name.
 */
#define DEFINE_CB(name) StringTreeRef name(CB_ARGS)

DEFINE_CB(NullProc) {
  return CXXCodeGen::makeVoidNode();
}

DEFINE_CB(EmptyProc) {
  return makeInnerNode(w.walkChildren(node, src));
}

DEFINE_CB(walkChildrenWithInsertingNewLines) {
  return foldWithSemicolon(w.walkChildren(node, src));
}

CodeBuilder::Procedure
handleBrackets(std::string opening,
    std::string closing,
    CodeBuilder::Procedure mainProc) {
  return [opening, closing, mainProc](CB_ARGS) {
    return makeTokenNode(opening) + mainProc(w, node, src)
        + makeTokenNode(closing);
  };
}

CodeBuilder::Procedure
handleBracketsLn(std::string opening,
    std::string closing,
    CodeBuilder::Procedure mainProc) {
  return [opening, closing, mainProc](CB_ARGS) {
    return makeTokenNode(opening) + mainProc(w, node, src)
        + makeTokenNode(closing) + makeNewLineNode();
  };
}

/*!
 * \brief Make a procedure that handles binary operation.
 * \param Operator Spelling of binary operator.
 */
CodeBuilder::Procedure
showBinOp(std::string Operator) {
  return [Operator](CB_ARGS) {
    xmlNodePtr lhs = findFirst(node, "*[1]", src.ctxt),
               rhs = findFirst(node, "*[2]", src.ctxt);
    return makeTokenNode("(") + w.walk(lhs, src) + makeTokenNode(Operator)
        + w.walk(rhs, src) + makeTokenNode(")");
  };
}

const CodeBuilder::Procedure EmptySNCProc = [](
    CB_ARGS) { return makeTokenNode(XMLString(xmlNodeGetContent(node))); };

/*!
 * \brief Make a procedure that outputs text content of a given
 * XML element.
 * \param prefix Text to output before text content.
 * \param suffix Text to output after text content.
 */
CodeBuilder::Procedure
showNodeContent(std::string prefix, std::string suffix) {
  return handleBrackets(prefix, suffix, EmptySNCProc);
}

/*!
 * \brief Make a procedure that processes the first child element of
 * a given XML element (At least one element should exist in it).
 * \param prefix Text to output before traversing descendant elements.
 * \param suffix Text to output after traversing descendant elements.
 */
CodeBuilder::Procedure
showChildElem(std::string prefix, std::string suffix) {
  return handleBrackets(prefix, suffix, EmptyProc);
}

/*!
 * \brief Make a procedure that handles unary operation.
 * \param Operator Spelling of unary operator.
 */
CodeBuilder::Procedure
showUnaryOp(std::string Operator) {
  return showChildElem(std::string("(") + Operator + "(", "))");
}

DEFINE_CB(postIncrExprProc) {
  return makeTokenNode("(") + makeInnerNode(w.walkChildren(node, src))
      + makeTokenNode("++)");
}

DEFINE_CB(postDecrExprProc) {
  return makeTokenNode("(") + makeInnerNode(w.walkChildren(node, src))
      + makeTokenNode("--)");
}

DEFINE_CB(castExprProc) {
  const auto dtident = getProp(node, "type");
  const auto Tstr = makeDecl(
      src.typeTable.at(dtident), makeVoidNode(), src.typeTable, src.nnsTable);
  const auto child = makeInnerNode(w.walkChildren(node, src));
  return wrapWithParen(wrapWithParen(Tstr) + wrapWithParen(child));
}

CodeBuilder::Procedure
handleIndentation(const CodeBuilder::Procedure mainProc) {
  return [mainProc](CB_ARGS) { return mainProc(w, node, src); };
}

const CodeBuilder::Procedure handleScope = handleBracketsLn(
    "{", "}", handleIndentation(walkChildrenWithInsertingNewLines));

DEFINE_CB(functionDefinitionProc) {
  const auto paramNames = getParamNames(node, src);
  auto acc = makeFunctionDeclHead(node, paramNames, src, true);

  if (auto ctorInitList =
          findFirst(node, "constructorInitializerList", src.ctxt)) {
    acc = acc + w.walk(ctorInitList, src);
  }

  auto body = findFirst(node, "body", src.ctxt);
  assert(body);
  acc = acc + makeTokenNode("{") + makeNewLineNode();
  acc = acc + w.walk(body, src);
  acc = acc + makeTokenNode("}");
  return wrapWithLangLink(acc, node, src);
}

DEFINE_CB(functionDeclProc) {
  const auto fnDtident = getProp(node, "type");
  const auto fnType =
      llvm::cast<XcodeMl::Function>(src.typeTable[fnDtident].get());
  auto decl = makeFunctionDeclHead(node, fnType->argNames(), src);
  decl = decl + makeTokenNode(";");
  return wrapWithLangLink(decl, node, src);
}

DEFINE_CB(varProc) {
  const auto name = makeTokenNode(getContent(node));
  const auto nnsident = getPropOrNull(node, "nns");
  if (!nnsident.hasValue()) {
    return name;
  }
  return makeNestedNameSpec(*nnsident, src) + name;
}

DEFINE_CB(emitInlineMemberFunctionDefinition) {
  const auto paramNames = getParamNames(node, src);
  auto acc = makeVoidNode();
  if (isTrueProp(node, "is_virtual", false)) {
    acc = acc + makeTokenNode("virtual");
  }
  if (isTrueProp(node, "is_static", false)) {
    acc = acc + makeTokenNode("static");
  }
  acc = acc + makeFunctionDeclHead(node, paramNames, src);

  if (auto ctorInitList =
          findFirst(node, "constructorInitializerList", src.ctxt)) {
    acc = acc + ProgramBuilder.walk(ctorInitList, src);
  }

  auto body = findFirst(node, "body", src.ctxt);
  assert(body);
  acc = acc + makeTokenNode("{");
  acc = acc + ProgramBuilder.walk(body, src);
  acc = acc + makeTokenNode("}");
  return acc;
}

DEFINE_CB(emitMemberFunctionDecl) {
  const auto fnDtident = getProp(node, "type");
  const auto fnType =
      llvm::cast<XcodeMl::Function>(src.typeTable[fnDtident].get());
  auto decl = makeVoidNode();
  if (isTrueProp(node, "is_virtual", false)) {
    decl = decl + makeTokenNode("virtual");
  }
  if (isTrueProp(node, "is_static", false)) {
    decl = decl + makeTokenNode("static");
  }
  decl = decl + makeFunctionDeclHead(node, fnType->argNames(), src);
  if (isTrueProp(node, "is_pure", false)) {
    decl = decl + makeTokenNode("=") + makeTokenNode("0");
  }
  decl = decl + makeTokenNode(";");
  return wrapWithLangLink(decl, node, src);
}

XcodeMl::CodeFragment
getNameFromMemberRefNode(xmlNodePtr node, const SourceInfo &src) {
  /* If the <memberRef> element has two children, use the second child. */
  const auto memberName = findFirst(node, "name", src.ctxt);
  if (memberName) {
    const auto name = getUnqualIdFromNameNode(memberName);
    return name->toString(src.typeTable, src.nnsTable);
  }
  /* Otherwise, it must have `member` attribute. */
  const auto name = getProp(node, "member");
  return makeTokenNode(name);
}

DEFINE_CB(memberRefProc) {
  const auto name = getNameFromMemberRefNode(node, src);
  return makeInnerNode(w.walkChildren(node, src)) + makeTokenNode("->") + name;
}

DEFINE_CB(memberAddrProc) {
  return makeTokenNode("&") + memberRefProc(w, node, src);
}

DEFINE_CB(memberPointerRefProc) {
  return makeInnerNode(w.walkChildren(node, src)) + makeTokenNode(".*")
      + makeTokenNode(getProp(node, "name"));
}

DEFINE_CB(compoundValueProc) {
  return makeTokenNode("{") + makeInnerNode(w.walkChildren(node, src))
      + makeTokenNode("}");
}

DEFINE_CB(thisExprProc) {
  return makeTokenNode("this");
}

DEFINE_CB(arrayRefExprProc) {
  auto arr = findFirst(node, "*[1]", src.ctxt),
       index = findFirst(node, "*[2]", src.ctxt);
  return w.walk(arr, src) + makeTokenNode("[") + w.walk(index, src)
      + makeTokenNode("]");
}

const auto compoundStatementProc = handleScope;

DEFINE_CB(whileStatementProc) {
  auto cond = findFirst(node, "condition", src.ctxt),
       body = findFirst(node, "body", src.ctxt);
  return makeTokenNode("while") + makeTokenNode("(") + w.walk(cond, src)
      + makeTokenNode(")") + handleScope(w, body, src);
}

DEFINE_CB(doStatementProc) {
  auto cond = findFirst(node, "condition", src.ctxt),
       body = findFirst(node, "body", src.ctxt);
  return makeTokenNode("do") + handleScope(w, body, src)
      + makeTokenNode("while") + makeTokenNode("(") + w.walk(cond, src)
      + makeTokenNode(")") + makeTokenNode(";");
}

DEFINE_CB(forStatementProc) {
  auto init = findFirst(node, "init", src.ctxt),
       cond = findFirst(node, "condition", src.ctxt),
       iter = findFirst(node, "iter", src.ctxt),
       body = findFirst(node, "body", src.ctxt);
  auto acc = makeTokenNode("for") + makeTokenNode("(");
  if (init) {
    acc = acc + w.walk(init, src);
  }
  acc = acc + makeTokenNode(";");
  if (cond) {
    acc = acc + w.walk(cond, src);
  }
  acc = acc + makeTokenNode(";");
  if (iter) {
    acc = acc + w.walk(iter, src);
  }
  acc = acc + makeTokenNode(")");
  return acc + handleScope(w, body, src);
}

DEFINE_CB(ifStatementProc) {
  auto cond = findFirst(node, "condition", src.ctxt),
       thenpart = findFirst(node, "then", src.ctxt),
       elsepart = findFirst(node, "else", src.ctxt);
  auto acc = makeTokenNode("if") + makeTokenNode("(");
  if (cond) {
    acc = acc + w.walk(cond, src);
  }
  acc = acc + makeTokenNode(") {");
  if (thenpart) {
    acc = acc + handleScope(w, thenpart, src);
  }
  if (elsepart) {
    acc = acc + makeTokenNode("} else {");
    acc = acc + handleScope(w, elsepart, src);
  }
  acc = acc + makeTokenNode("}");
  return acc;
}

DEFINE_CB(switchStatementProc) {
  auto cond = findFirst(node, "value", src.ctxt);
  auto body = findFirst(node, "body", src.ctxt);
  return makeTokenNode("switch") + wrapWithParen(w.walk(cond, src))
      + makeTokenNode("{") + insertNewLines(w.walkChildren(body, src))
      + makeTokenNode("}");
}

DEFINE_CB(caseLabelProc) {
  auto value = findFirst(node, "value", src.ctxt);
  return makeTokenNode("case") + makeInnerNode(w.walkChildren(value, src))
      + makeTokenNode(":");
}

DEFINE_CB(defaultLabelProc) {
  return makeTokenNode("default") + makeTokenNode(":");
}

DEFINE_CB(returnStatementProc) {
  xmlNodePtr child = xmlFirstElementChild(node);
  if (child) {
    return makeTokenNode("return") + w.walk(child, src) + makeTokenNode(";");
  } else {
    return makeTokenNode("return") + makeTokenNode(";");
  }
}

DEFINE_CB(exprStatementProc) {
  const auto showChild = showChildElem("", ";");
  return showChild(w, node, src);
}

DEFINE_CB(functionCallProc) {
  xmlNodePtr arguments = findFirst(node, "arguments", src.ctxt);

  if (const auto opNode = findFirst(node, "operator", src.ctxt)) {
    const auto op = makeOpNode(opNode);
    return makeTokenNode("operator") + op + w.walk(arguments, src);
  }

  xmlNodePtr function = findFirst(node, "function|memberFunction", src.ctxt);
  if (!function) {
    std::cerr << "error: callee not found" << getXcodeMlPath(node)
              << std::endl;
    std::abort();
  }
  const auto callee = findFirst(function, "*", src.ctxt);
  return w.walk(callee, src) + w.walk(arguments, src);
}

DEFINE_CB(memberFunctionCallProc) {
  const auto function = findFirst(node, "*[1]", src.ctxt);
  const auto arguments = findFirst(node, "arguments", src.ctxt);
  return w.walk(function, src) + w.walk(arguments, src);
}

DEFINE_CB(valueProc) {
  const auto child = findFirst(node, "*", src.ctxt);
  if (getName(child) == "value") {
    // aggregate (See {#sec:program.value})
    const auto grandchildren = w.walkChildren(child, src);
    return wrapWithBrace(join(",", grandchildren));
  }
  return w.walk(child, src);
}

DEFINE_CB(newExprProc) {
  const auto type = src.typeTable.at(getProp(node, "type"));
  // FIXME: Support scalar type
  const auto pointeeT =
      llvm::cast<XcodeMl::Pointer>(type.get())->getPointee(src.typeTable);
  const auto NewTypeId =
      pointeeT->makeDeclaration(makeVoidNode(), src.typeTable, src.nnsTable);
  /* Ref: [new.expr]/4
   * new int(*[10])();   // error
   * new (int(*[10])()); // OK
   * new int;            // OK
   * new (int);          // OK
   * new ((int));        // error
   */
  const auto arguments = findFirst(node, "arguments", src.ctxt);

  return makeTokenNode("new")
      + (hasParen(pointeeT, src.typeTable) ? wrapWithParen(NewTypeId)
                                           : NewTypeId)
      + (arguments ? w.walk(arguments, src) : makeVoidNode());
}

DEFINE_CB(newArrayExprProc) {
  const auto type = src.typeTable.at(getProp(node, "type"));
  const auto pointeeT =
      llvm::cast<XcodeMl::Pointer>(type.get())->getPointee(src.typeTable);
  const auto size_expr = w.walk(findFirst(node, "size", src.ctxt), src);
  const auto decl = pointeeT->makeDeclaration(
      wrapWithSquareBracket(size_expr), src.typeTable, src.nnsTable);
  return makeTokenNode("new") + wrapWithParen(decl);
  /* new int(*[10])();   // error
   * new (int(*[10])()); // OK
   */
}

DEFINE_CB(argumentsProc) {
  auto acc = makeTokenNode("(");
  bool alreadyPrinted = false;
  for (xmlNodePtr arg = node->children; arg; arg = arg->next) {
    if (arg->type != XML_ELEMENT_NODE) {
      continue;
    }
    if (alreadyPrinted) {
      acc = acc + makeTokenNode(",");
    }
    acc = acc + w.walk(arg, src);
    alreadyPrinted = true;
  }
  return acc + makeTokenNode(")");
}

DEFINE_CB(condExprProc) {
  xmlNodePtr prd = findFirst(node, "*[1]", src.ctxt),
             second = findFirst(node, "*[2]", src.ctxt),
             third = findFirst(node, "*[3]", src.ctxt);
  if (third) {
    return makeTokenNode("(") + w.walk(prd, src) + makeTokenNode("?")
        + w.walk(second, src) + makeTokenNode(":") + w.walk(third, src)
        + makeTokenNode(")");
  } else {
    return makeTokenNode("(") + w.walk(prd, src) + makeTokenNode("?:")
        + w.walk(second, src) + makeTokenNode(")");
  }
}

DEFINE_CB(addrOfExprProc) {
  if (isTrueProp(node, "is_expedient", false)) {
    auto expr = findFirst(node, "*[1]", src.ctxt);
    return w.walk(expr, src);
  }
  const auto wrap = showUnaryOp("&");
  return wrap(w, node, src);
}

} // namespace

XcodeMl::CodeFragment
declareClassTypeInit(
    const CodeBuilder &w, xmlNodePtr ctorExpr, SourceInfo &src) {
  auto copySrc = findFirst(
      ctorExpr, "clangStmt[@class='MaterializeTemporaryExpr']", src.ctxt);
  if (copySrc) {
    /* Use `=` to reduce ambiguity.
     * A a(A());    // function
     * A a = A();   // class
     */
    return makeTokenNode("=") + w.walk(copySrc, src);
  }
  const auto args = w.walkChildren(ctorExpr, src);
  /*
   * X a();  // function ([dcl.init]/6)
   * X a;    // class
   */
  return args.empty() ? makeVoidNode()
                      : wrapWithParen(cxxgen::join(",", args));
}

namespace {

DEFINE_CB(varDeclProc) {
  const auto nameNode = findFirst(node, "name", src.ctxt);
  const auto name = getUnqualIdFromNameNode(nameNode);

  const auto dtident = getProp(node, "type");
  const auto type = src.typeTable.at(dtident);

  auto acc = makeVoidNode();
  acc = acc + makeDecl(type,
                  name->toString(src.typeTable, src.nnsTable),
                  src.typeTable,
                  src.nnsTable);
  xmlNodePtr valueElem = findFirst(node, "value", src.ctxt);
  if (!valueElem) {
    return wrapWithLangLink(acc + makeTokenNode(";"), node, src);
  }

  auto ctorExpr =
      findFirst(valueElem, "clangStmt[@class='CXXConstructExpr']", src.ctxt);
  if (ctorExpr) {
    const auto decl =
        acc + declareClassTypeInit(w, ctorExpr, src) + makeTokenNode(";");
    return wrapWithLangLink(decl, node, src);
  }

  acc = acc + makeTokenNode("=") + w.walk(valueElem, src) + makeTokenNode(";");
  return wrapWithLangLink(acc, node, src);
}

DEFINE_CB(emitDataMemberDecl) {
  const auto nameNode = findFirst(node, "name", src.ctxt);
  const auto name = getUnqualIdFromNameNode(nameNode);

  const auto dtident = getProp(node, "type");
  const auto type = src.typeTable.at(dtident);

  auto acc = makeVoidNode();
  if (isTrueProp(node, "is_static_data_member", false)) {
    acc = acc + makeTokenNode("static");
  }
  acc = acc + makeDecl(type,
                  name->toString(src.typeTable, src.nnsTable),
                  src.typeTable,
                  src.nnsTable);
  xmlNodePtr valueElem = findFirst(node, "value", src.ctxt);
  if (!valueElem) {
    return wrapWithLangLink(acc + makeTokenNode(";"), node, src);
  }

  auto ctorExpr =
      findFirst(valueElem, "clangStmt[@class='CXXConstructExpr']", src.ctxt);
  if (ctorExpr) {
    const auto decl =
        acc + declareClassTypeInit(w, ctorExpr, src) + makeTokenNode(";");
    return wrapWithLangLink(decl, node, src);
  }

  acc = acc + makeTokenNode("=") + ProgramBuilder.walk(valueElem, src)
      + makeTokenNode(";");
  return wrapWithLangLink(acc, node, src);
}

XcodeMl::CodeFragment
getCtorInitName(xmlNodePtr node, const XcodeMl::TypeTable &env) {
  const auto dataMember = getPropOrNull(node, "member");
  if (dataMember.hasValue()) {
    return makeTokenNode(*dataMember);
  }
  const auto base = getType(node);
  // parampack.src.cpp error
  const auto T = env[base];
  const auto classT = llvm::dyn_cast<XcodeMl::ClassType>(T.get());
  XcodeMl::CodeFragment name = makeVoidNode();
  if(classT)
    name = name + classT->name();
  //  const auto name = classT->name();
  return name;
}

DEFINE_CB(ctorInitProc) {
  const auto member = getCtorInitName(node, src.typeTable);
  const auto expr = findFirst(node, "clangStmt[1]", src.ctxt);
  if (!expr) {
    return member + wrapWithParen(makeVoidNode());
  }
  if (auto constructExpr
      = findFirst(node, "clangStmt[@class='CXXConstructExpr']", src.ctxt)) {
    const auto constructExprParams = createNodes(constructExpr, "clangStmt", w, src);
    return member + wrapWithParen(join(",", constructExprParams));
  }
  return member + makeTokenNode("(") + w.walk(expr, src) + makeTokenNode(")");
}

DEFINE_CB(accessToAnonRecordExprProc) {
  const auto baseName = getProp(node, "member");
  const auto nnsident = getPropOrNull(node, "nns");
  return (nnsident.hasValue() ? makeNestedNameSpec(*nnsident, src)
                              : makeVoidNode())
      + makeTokenNode(baseName);
}

DEFINE_CB(clangStmtProc) {
  return ClangStmtHandler.walk(node, w, src);
}

DEFINE_CB(clangDeclProc) {
  return ClangDeclHandler.walk(node, w, src);
}

DEFINE_CB(classScopeClangDeclProc) {
  return ClangDeclHandlerInClass.walk(node, w, src);
}

DEFINE_CB(clangTypeLocProc) {
  const auto T = src.typeTable.at(getType(node));
  return T->makeDeclaration(
      CXXCodeGen::makeVoidNode(), src.typeTable, src.nnsTable);
}

DEFINE_CB(clangNestedNameSpecProc) {
  return ClangNestedNameSpecHandler.walk(node, src);
}

} // namespace

const CodeBuilder ProgramBuilder("ProgramBuilder",
    makeInnerNode,
    {
        std::make_tuple("typeTable", NullProc),
        std::make_tuple("functionDefinition", functionDefinitionProc),
        std::make_tuple("functionDecl", functionDeclProc),
        std::make_tuple("intConstant", EmptySNCProc),
        std::make_tuple("floatConstant", EmptySNCProc),
        std::make_tuple("moeConstant", EmptySNCProc),
        std::make_tuple("booleanConstant", EmptySNCProc),
        std::make_tuple("funcAddr", EmptySNCProc),
        std::make_tuple("stringConstant", showNodeContent("\"", "\"")),
        std::make_tuple("Var", varProc),
        std::make_tuple("varAddr", showNodeContent("(&", ")")),
        std::make_tuple("pointerRef", showUnaryOp("*")),
        std::make_tuple("memberRef", memberRefProc),
        std::make_tuple("memberAddr", memberAddrProc),
        std::make_tuple("memberPointerRef", memberPointerRefProc),
        std::make_tuple("compoundValue", compoundValueProc),
        std::make_tuple("compoundStatement", compoundStatementProc),
        std::make_tuple("whileStatement", whileStatementProc),
        std::make_tuple("doStatement", doStatementProc),
        std::make_tuple("forStatement", forStatementProc),
        std::make_tuple("ifStatement", ifStatementProc),
        std::make_tuple("switchStatement", switchStatementProc),
        std::make_tuple("caseLabel", caseLabelProc),
        std::make_tuple("defaultLabel", defaultLabelProc),
        std::make_tuple("thisExpr", thisExprProc),
        std::make_tuple("arrayRef", arrayRefExprProc),
        std::make_tuple("assignExpr", showBinOp(" = ")),
        std::make_tuple("plusExpr", showBinOp(" + ")),
        std::make_tuple("minusExpr", showBinOp(" - ")),
        std::make_tuple("mulExpr", showBinOp(" * ")),
        std::make_tuple("divExpr", showBinOp(" / ")),
        std::make_tuple("modExpr", showBinOp(" % ")),
        std::make_tuple("LshiftExpr", showBinOp(" << ")),
        std::make_tuple("RshiftExpr", showBinOp(" >> ")),
        std::make_tuple("logLTExpr", showBinOp(" < ")),
        std::make_tuple("logGTExpr", showBinOp(" > ")),
        std::make_tuple("logLEExpr", showBinOp(" <= ")),
        std::make_tuple("logGEExpr", showBinOp(" >= ")),
        std::make_tuple("logEQExpr", showBinOp(" == ")),
        std::make_tuple("logNEQExpr", showBinOp(" != ")),
        std::make_tuple("bitAndExpr", showBinOp(" & ")),
        std::make_tuple("bitXorExpr", showBinOp(" ^ ")),
        std::make_tuple("bitOrExpr", showBinOp(" | ")),
        std::make_tuple("logAndExpr", showBinOp(" && ")),
        std::make_tuple("logOrExpr", showBinOp(" || ")),
        std::make_tuple("asgMulExpr", showBinOp(" *= ")),
        std::make_tuple("asgDivExpr", showBinOp(" /= ")),
        std::make_tuple("asgPlusExpr", showBinOp(" += ")),
        std::make_tuple("asgMinusExpr", showBinOp(" -= ")),
        std::make_tuple("asgLshiftExpr", showBinOp(" <<= ")),
        std::make_tuple("asgRshiftExpr", showBinOp(" >>= ")),
        std::make_tuple("asgBitAndExpr", showBinOp(" &= ")),
        std::make_tuple("asgBitOrExpr", showBinOp(" |= ")),
        std::make_tuple("asgBitXorExpr", showBinOp(" ^= ")),
        std::make_tuple("unaryPlusExpr", showUnaryOp("+")),
        std::make_tuple("unaryMinusExpr", showUnaryOp("-")),
        std::make_tuple("preIncrExpr", showUnaryOp("++")),
        std::make_tuple("preDecrExpr", showUnaryOp("--")),
        std::make_tuple("postIncrExpr", postIncrExprProc),
        std::make_tuple("postDecrExpr", postDecrExprProc),
        std::make_tuple("castExpr", castExprProc),
        std::make_tuple("AddrOfExpr", addrOfExprProc),
        std::make_tuple("pointerRef", showUnaryOp("*")),
        std::make_tuple("bitNotExpr", showUnaryOp("~")),
        std::make_tuple("logNotExpr", showUnaryOp("!")),
        std::make_tuple("sizeOfExpr", showUnaryOp("sizeof")),
        std::make_tuple("AlignOfExpr", showUnaryOp("alignof")),
	std::make_tuple("gccAlignOfExpr", showUnaryOp("__alignof")),
        std::make_tuple("newExpr", newExprProc),
        std::make_tuple("newArrayExpr", newArrayExprProc),
        std::make_tuple("functionCall", functionCallProc),
        std::make_tuple("memberFunctionCall", memberFunctionCallProc),
        std::make_tuple("arguments", argumentsProc),
        std::make_tuple("condExpr", condExprProc),
        std::make_tuple("exprStatement", exprStatementProc),
        std::make_tuple("returnStatement", returnStatementProc),
        std::make_tuple("varDecl", varDeclProc),
        std::make_tuple("value", valueProc),

        /* out of specification */
        std::make_tuple(
            "xcodemlAccessToAnonRecordExpr", accessToAnonRecordExprProc),

        /* for elements defined by clang */
        std::make_tuple("clangStmt", clangStmtProc),
        std::make_tuple("clangDecl", clangDeclProc),
        std::make_tuple("clangTypeLoc", clangTypeLocProc),
        std::make_tuple("clangNestedNameSpecifier", clangNestedNameSpecProc),
        std::make_tuple("clangConstructorInitializer", ctorInitProc),

        /* for CtoXcodeML */
        std::make_tuple("Decl_Record", NullProc),
        // Ignore Decl_Record (structs are already emitted)
    });

namespace {

Language
getSourceLanguage(xmlNodePtr rootNode, xmlXPathContextPtr ctxt) {
  const auto topNode = findFirst(rootNode, "/*", ctxt);

  const auto lang = getPropOrNull(topNode, "language");
  if (!lang.hasValue()) {
    // The default value of `language` attribute is "C++"
    return Language::CPlusPlus;
  }

  if (*lang == "C++") {
    return Language::CPlusPlus;
  } else if (*lang == "C") {
    return Language::C;
  } else {
    return Language::Invalid;
  }
}

} // namespace

const CodeBuilder ClassDefinitionBuilder("ClassDefinitionBuilder",
    makeInnerNode,
    {
        std::make_tuple("functionDecl", emitMemberFunctionDecl),
        std::make_tuple(
            "functionDefinition", emitInlineMemberFunctionDefinition),
        std::make_tuple("varDecl", emitDataMemberDecl),

        /* for elements defined by clang */
        std::make_tuple("clangDecl", classScopeClangDeclProc),
        std::make_tuple("clangStmt", clangStmtProc),
    });

namespace {

void
readXcodeProgram(
    xmlNodePtr rootNode, xmlXPathContextPtr ctxt, std::stringstream &ss) {
  xmlNodePtr typeTableNode =
      findFirst(rootNode, "/XcodeProgram/typeTable", ctxt);
  xmlNodePtr nnsTableNode =
      findFirst(rootNode, "/XcodeProgram/nnsTable", ctxt);
  SourceInfo src(ctxt,
      parseTypeTable(typeTableNode, ctxt, ss),
      analyzeNnsTable(nnsTableNode, ctxt),
      getSourceLanguage(rootNode, ctxt));

  cxxgen::Stream out;
  xmlNodePtr globalDeclarations =
      findFirst(rootNode, "/XcodeProgram/globalDeclarations", src.ctxt);
  separateByBlankLines(ProgramBuilder.walkChildren(globalDeclarations, src))
      ->flush(out);

  ss << out.str();
}

void
readClangAST(
    xmlNodePtr rootNode, xmlXPathContextPtr ctxt, std::stringstream &ss) {
  xmlNodePtr typeTableNode =
      findFirst(rootNode, "/clangAST/clangDecl/xcodemlTypeTable", ctxt);
  xmlNodePtr nnsTableNode =
      findFirst(rootNode, "/clangAST/clangDecl/xcodemlNnsTable", ctxt);
  SourceInfo src(ctxt,
      parseTypeTable(typeTableNode, ctxt, ss),
      analyzeNnsTable(nnsTableNode, ctxt),
      getSourceLanguage(rootNode, ctxt));

  cxxgen::Stream out;
  if (src.language == Language::CPlusPlus) {
    out << "template<typename T>"
           "struct __xcodeml_identity { typedef T t; };"
        << CXXCodeGen::newline;
  }
  xmlNodePtr decl = findFirst(rootNode, "/clangAST/clangDecl", src.ctxt);
  const auto program = ClangDeclHandler.walk(decl, ProgramBuilder, src);
  program->flush(out);

  ss << out.str();
}

} // namespace

/*!
 * \brief Traverse an XcodeML document and generate C++ source code.
 * \param[in] doc XcodeML document.
 * \param[out] ss Stringstream to flush C++ source code.
 */
void
buildCode(
    xmlNodePtr rootNode, xmlXPathContextPtr ctxt, std::stringstream &ss) {
  const auto docType = getName(rootNode);
  if (std::equal(docType.cbegin(), docType.cend(), "XcodeProgram")) {
    readXcodeProgram(rootNode, ctxt, ss);
    return;
  } else if (std::equal(docType.cbegin(), docType.cend(), "clangAST")) {
    readClangAST(rootNode, ctxt, ss);
  } else {
    std::cerr << "error: unknown document type" << std::endl;
    std::abort();
  }
}
