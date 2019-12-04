#include "XMLVisitorBase.h"

#include "TypeTableInfo.h"
#include "NnsTableInfo.h"
#include "DeclarationsVisitor.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Driver/Options.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/Signals.h"

#include <libxml/xmlsave.h>
#include <time.h>
#include <iostream>
#include <string>

using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;
using namespace llvm;

static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static std::unique_ptr<opt::OptTable> Options(createDriverOptTable());

namespace {

const char *
getLanguageString(const LangOptions &Opts) {
  if (Opts.CPlusPlus) {
    return "C++";
  } else if (Opts.C99 || Opts.C11) {
    return "C";
  } else {
    std::cout << "Invalid file" << std::endl;
    std::abort();
  }
}

} // namespace

class XMLASTConsumer : public ASTConsumer {
  xmlNodePtr rootNode;

public:
  explicit XMLASTConsumer(xmlNodePtr N) : rootNode(N){};

  virtual void
  HandleTranslationUnit(ASTContext &CXT) override {
    MangleContext *MC = CXT.createMangleContext();
    InheritanceInfo inheritanceinfo;
    InheritanceInfo *II = &inheritanceinfo;
    DeclarationsVisitorContext DVC(MC, II);
    DeclarationsVisitor DV(MC, rootNode, nullptr, &DVC);
    Decl *D = CXT.getTranslationUnitDecl();
    
    DV.TraverseDecl(D);
  }
#if 0
    virtual bool HandleTopLevelDecl(DeclGroupRef DG) override {
        // We can check whether parsing should be continued or not
        // at the time that each declaration parsing is done.
        // default: true.
        return true;
    }
#endif
};

class XMLASTDumpAction : public ASTFrontendAction {
private:
  xmlDocPtr xmlDoc;

public:
  bool
  BeginSourceFileAction(
      clang::CompilerInstance &CI) override {
    xmlDoc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr rootnode = xmlNewNode(nullptr, BAD_CAST "clangAST");
    xmlDocSetRootElement(xmlDoc, rootnode);

    char strftimebuf[BUFSIZ];
    time_t t = time(nullptr);

    strftime(strftimebuf, sizeof strftimebuf, "%F %T", localtime(&t));
    StringRef Filename = getCurrentFile();

    xmlNewProp(rootnode, BAD_CAST "source", BAD_CAST Filename.data());
    xmlNewProp(rootnode,
        BAD_CAST "language",
        BAD_CAST getLanguageString(CI.getLangOpts()));
    xmlNewProp(rootnode, BAD_CAST "time", BAD_CAST strftimebuf);

    return true;
  };

  virtual std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &CI, StringRef file) override {
    (void)CI; // suppress warnings
    (void)file; // suppress warnings

    std::unique_ptr<ASTConsumer> C(
        new XMLASTConsumer(xmlDocGetRootElement(xmlDoc)));
    return C;
  }

  void
  EndSourceFileAction(void) override {
    // int saveopt = XML_SAVE_FORMAT | XML_SAVE_NO_EMPTY;
    int saveopt = XML_SAVE_FORMAT;
    xmlSaveCtxtPtr ctxt = xmlSaveToFilename("-", "UTF-8", saveopt);
    xmlSaveDoc(ctxt, xmlDoc);
    xmlSaveClose(ctxt);
    xmlFreeDoc(xmlDoc);
  }
};

int
main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  CommonOptionsParser OptionsParser(argc, argv, CXX2XMLCategory);
  ClangTool Tool(
      OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
  Tool.appendArgumentsAdjuster(clang::tooling::getClangSyntaxOnlyAdjuster());

  std::unique_ptr<FrontendActionFactory> FrontendFactory =
      newFrontendActionFactory<XMLASTDumpAction>();
  return Tool.run(FrontendFactory.get());
}

///
/// Local Variables:
/// indent-tabs-mode: nil
/// c-basic-offset: 4
/// End:
///
