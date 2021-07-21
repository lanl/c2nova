/***********************************
 * Translate C code to Nova macros *
 * By Scott Pakin <pakin@lanl.gov> *
 ***********************************/

#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Refactoring.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <unistd.h>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

// Match various operations and convert these to Nova.
class C_to_Nova : public clang::ast_matchers::MatchFinder::MatchCallback {
private:
  // Define some shorthand for a map from filename to replacement list.
  using repl_map_t = std::map<std::string, clang::tooling::Replacements>;

  // List of replacements to make.
  repl_map_t& replacements;

public:
  // Store the set of replacements we were given to modify.
  explicit C_to_Nova(repl_map_t& repls) : replacements(repls) {}

  // Add a set of matchers to a finder.
  void add_matchers(MatchFinder& mfinder) {
    mfinder.addMatcher(integerLiteral().bind("int-lit"), this);
  }

  // Process all of our matches.
  virtual void run(const MatchFinder::MatchResult& mresult) {
    const IntegerLiteral* intLit = mresult.Nodes.getNodeAs<IntegerLiteral>("int-lit");

    // Temporary
    llvm::errs() << "Look what I got:\n";
    intLit->dump();
  }
};

// Prepare --help to output some helpful information.
static llvm::cl::OptionCategory c2n_opts("cpp2nova options");
//static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

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
  auto opt_parser = CommonOptionsParser::create(argc, argv, c2n_opts, llvm::cl::OneOrMore);
  if (!opt_parser) {
    llvm::errs() << opt_parser.takeError();
    return 1;
  }

  // Instantiate and prepare our Clang tool.
  RefactoringTool tool(opt_parser->getCompilations(), opt_parser->getSourcePathList());
  C_to_Nova c2n(tool.getReplacements());
  MatchFinder mfinder;
  c2n.add_matchers(mfinder);

  // Run our Clang tool.
  tool.run(newFrontendActionFactory(&mfinder).get());
  return 0;
}
