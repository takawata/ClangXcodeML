#include <functional>
#include <sstream>
#include <memory>
#include <map>
#include <cassert>
#include <vector>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include "llvm/Support/Casting.h"
#include "LibXMLUtil.h"
#include "XMLString.h"
#include "XMLWalker.h"
#include "AttrProc.h"
#include "StringTree.h"
#include "Symbol.h"
#include "XcodeMlType.h"
#include "XcodeMlEnvironment.h"
#include "SourceInfo.h"
#include "SymbolAnalyzer.h"

using SymbolAnalyzer = AttrProc<
  void,
  xmlXPathContextPtr,
  XcodeMl::Environment&>;

using CXXCodeGen::makeTokenNode;

#define SA_ARGS xmlNodePtr node __attribute__((unused)), \
                xmlXPathContextPtr ctxt __attribute__((unused)), \
                XcodeMl::Environment & map __attribute__((unused))

#define DEFINE_SA(name) static void name(SA_ARGS)

DEFINE_SA(tagnameProc) {
  const auto dataTypeIdent = getProp(node, "type");
  auto typeref = map.at(dataTypeIdent); // structType must exists
  auto structType = llvm::cast<XcodeMl::Struct>(typeref.get());
  xmlNodePtr nameNode(findFirst(node, "name|operator", ctxt));
  XMLString tag(xmlNodeGetContent(nameNode));
  structType->setTagName(
      makeTokenNode(getNameFromIdNode(node, ctxt)));
}

const SymbolAnalyzer CXXSymbolAnalyzer (
    "sclass",
    {
      { "tagname", tagnameProc },
    });

void analyzeSymbols(
    xmlNodePtr node,
    xmlXPathContextPtr ctxt,
    XcodeMl::Environment& map
) {
  CXXSymbolAnalyzer.walkAll(node, ctxt, map);
}
