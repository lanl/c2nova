/***********************************
 * Translate C code to Nova macros *
 * By Scott Pakin <pakin@lanl.gov> *
 ***********************************/

#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <unistd.h>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

StatementMatcher LoopMatcher =
  forStmt(hasLoopInit(declStmt(hasSingleDecl(varDecl(
						     hasInitializer(integerLiteral(equals(0)))))))).bind("forLoop");

class LoopPrinter : public MatchFinder::MatchCallback {
public :
  virtual void run(const MatchFinder::MatchResult &Result) {
    if (const ForStmt *FS = Result.Nodes.getNodeAs<clang::ForStmt>("forLoop"))
      FS->dump();
  }
};

// Prepare --help to output some helpful information.
static llvm::cl::OptionCategory C2NToolCategory("cpp2nova options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// As a user, I hate having to append "--" to the command line when running a
// Clang tool.  If we don't see a "--", append it ourself.
bool append_ddash(int argc, const char **argv) {
  // Return true if the command line already contains a double-dash.
  for (int i = 1; i < argc; i++)
    if (std::string(argv[i]) == "--")
      return true;

  // No double dash: append one and restart the program.
  char **new_argv = new char *[argc + 2];
  for (int i = 0; i < argc; i++)
    new_argv[i] = strdup(argv[i]);
  new_argv[argc] = strdup("--");
  new_argv[argc + 1] = nullptr;
  if (execvp(argv[0], new_argv) == -1)
    return false;
  return true; // We should never get here.
}

int main(int argc, const char **argv) {
  // Append a "--" to the command line if none is already present.
  if (!append_ddash(argc, argv)) {
    llvm::errs() << "failed to restart " << argv[0] << "("
		 << std::strerror(errno) << ")\n";
    return 1;
  }

  // Parse the command line.
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, C2NToolCategory, llvm::cl::OneOrMore);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();

  // Run our tool on the specified source file.
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  LoopPrinter Printer;
  MatchFinder Finder;
  Finder.addMatcher(LoopMatcher, &Printer);
  return Tool.run(newFrontendActionFactory(&Finder).get());
}
