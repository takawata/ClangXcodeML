#include "XcodeMlNameElem.h"

xmlNodePtr
makeNameNode(
    TypeTableInfo&,
    const NamedDecl* ND)
{
  xmlNodePtr node = nullptr;
  if (auto MD = dyn_cast<CXXMethodDecl>(ND)) {
    node = makeNameNodeForCXXMethodDecl(MD);
  } else {
    node = xmlNewNode(nullptr, BAD_CAST "name");
    xmlNodeSetContent(node, BAD_CAST (ND->getNameAsString().c_str()));
  }

  if (ND->isLinkageValid()) {
    const auto FL = ND->getFormalLinkage(),
               LI = ND->getLinkageInternal();
    xmlNewProp(
        node,
        BAD_CAST "linkage",
        stringifyLinkage(FL));
    if (FL != LI) {
      xmlNewProp(
          node,
          BAD_CAST "clang_linkage_internal",
          stringifyLinkage(LI));
    }
  } else {
    // should not be executed
    xmlNewProp(
        node,
        BAD_CAST "clang_has_invalid_linkage",
        BAD_CAST "true");
  }

  return node;
}

xmlNodePtr
makeNameNodeForCXXMethodDecl(
    TypeTableInfo&,
    const CXXMethodDecl* MD)
{
  auto nameNode = xmlNewNode(nullptr, BAD_CAST "name");
  if (isa<CXXConstructorDecl>(MD)) {
    xmlNewProp(
        nameNode,
        BAD_CAST "name_kind",
        BAD_CAST "constructor");
    return nameNode;
  } else if (isa<CXXDestructorDecl>(MD)) {
    xmlNewProp(
        nameNode,
        BAD_CAST "name_kind",
        BAD_CAST "destructor");
    return nameNode;
  } else if (auto OOK = MD->getOverloadedOperator()) {
    xmlNewProp(
        nameNode,
        BAD_CAST "name_kind",
        BAD_CAST "operator");
    xmlNodeAddContent(
        nameNode,
        BAD_CAST OverloadedOperatorKindToString(OOK, MD->param_size()));
    return nameNode;
  }
  const auto ident = MD->getIdentifier();
  assert(ident);
  xmlNodeAddContent(nameNode, BAD_CAST ident->getName().data());
  xmlNewProp(
      nameNode,
      BAD_CAST "name_kind",
      BAD_CAST "name");
  return nameNode;
}

xmlNodePtr
makeIdNodeForCXXMethodDecl(
    TypeTableInfo& TTI,
    const CXXMethodDecl* method)
{
  auto idNode = xmlNewNode(nullptr, BAD_CAST "id");
  xmlNewProp(
      idNode,
      BAD_CAST "type",
      BAD_CAST TTI.getTypeName(method->getType()).c_str());
  auto nameNode = makeNameNodeForCXXMethodDecl(TTI, method);
  xmlAddChild(idNode, nameNode);
  return idNode;
}

