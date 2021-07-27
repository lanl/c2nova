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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;
namespace fs = std::filesystem;

// Prepare --help to output some helpful information.
static llvm::cl::OptionCategory c2n_opts("cpp2nova options");
static cl::opt<std::string> outfile("o", cl::desc("specify output filename"), cl::value_desc("output.c"));
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// Match various operations and convert these to Nova.
class C_to_Nova : public clang::ast_matchers::MatchFinder::MatchCallback {
private:
  // Define some shorthand for a map from filename to replacement list.
  using repl_map_t = std::map<std::string, clang::tooling::Replacements>;

  // List of replacements to make.
  repl_map_t& replacements;

  // Return the end of the last token in a source range.
  SourceLocation get_end_of_end(SourceManager& sm, SourceRange& sr) {
    SourceLocation end_tok_begin(sr.getEnd());
    LangOptions lopt;
    SourceLocation end_tok_end(Lexer::getLocForEndOfToken(end_tok_begin, 0, sm, lopt));
    return end_tok_end;
  }

  // Return the text corresponding to a source range.
  std::string get_text(SourceManager& sm, SourceRange& sr) {
    SourceLocation ofs0(sr.getBegin());
    SourceLocation ofs1(get_end_of_end(sm, sr));
    const char* ptr0(sm.getCharacterData(ofs0));
    const char* ptr1(sm.getCharacterData(ofs1));
    return std::string(ptr0, ptr1);
  }

  // Return an identifier found at a given location.
  std::string get_ident(SourceManager& sm, SourceLocation id_begin) {
    LangOptions lopt;
    SourceLocation id_end(Lexer::getLocForEndOfToken(id_begin, 0, sm, lopt));
    const char* ptr0(sm.getCharacterData(id_begin));
    const char* ptr1(sm.getCharacterData(id_end));
    return std::string(ptr0, ptr1);
  }

  // Wrap integer/float variable declarations with "DeclareApeVar" or
  // "DeclareApeVarInit", as appropriate.  This serves a helper function for
  // process_integer_decl() and process_float_decl().
  void process_var_decl(const MatchFinder::MatchResult& mresult,
                        const StringRef bind_name,
                        const std::string nova_type) {
    // Extract the declaration in both raw and textual forms.
    const Decl* decl = mresult.Nodes.getNodeAs<Decl>(bind_name);
    if (decl == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(decl->getSourceRange());
    SourceLocation ofs0(sr.getBegin());
    std::string text(get_text(sm, sr));

    // Look for magic comments to control allocation.
    StringRef where("Ape");
    StringRef what("Var");
    ASTContext& ctx = decl->getASTContext();
    const RawComment* raw_cmt = ctx.getRawCommentForDeclNoCache(decl);
    if (raw_cmt != nullptr) {
      StringRef comment = raw_cmt->getRawText(sm);
      if (comment.contains("[CU]"))
        where = StringRef("CU");
      if (comment.contains("[mem]"))
        what = StringRef("Mem");
    }
    std::string declare(std::string("Declare") + where.str() + what.str());

    // Extract the variable name.
    std::string var_name(get_ident(sm, decl->getLocation()));

    // Generate a replacement either with or without an initializer.
    const Expr* rhs = mresult.Nodes.getNodeAs<Expr>("rhs");
    std::string fname(sm.getFilename(ofs0).str());
    if (rhs == nullptr) {
      // No initializer.
      Replacement rep(sm, ofs0, text.length(), declare + "(" + var_name + ", " + nova_type + ")");
      if (replacements[fname].add(rep))
        llvm::errs() << "failed to perform replacement: " << rep.toString() << "\n";
    }
    else {
      // Initializer.  The trick here is not to replace the initializer itself,
      // or Clang will complain about overlapping replacements.  We therefore
      // replace only the text appearing either before or after the
      // initializer.
      const char* ptr0(sm.getCharacterData(ofs0));
      SourceRange rhs_sr(rhs->getSourceRange());
      SourceLocation rhs_ofs0(rhs_sr.getBegin());
      const char* rhs_ptr0(sm.getCharacterData(rhs_ofs0));
      Replacement rep1(sm, ofs0, rhs_ptr0 - ptr0, declare + "Init(" + var_name + ", " + nova_type + ", ");
      if (replacements[fname].add(rep1))
        llvm::errs() << "failed to perform replacement: " << rep1.toString() << "\n";
      SourceLocation ofs1(get_end_of_end(sm, sr));
      Replacement rep2(sm, ofs1, 0, ")");
      if (replacements[fname].add(rep2))
        llvm::errs() << "failed to perform replacement: " << rep2.toString() << "\n";
    }
  }

  // Wrap integer variable declarations with "DeclareApeVar" or "DeclareApeVarInit".
  void process_int_var_decl(const MatchFinder::MatchResult& mresult) {
    process_var_decl(mresult, StringRef("int-decl"), std::string("Int"));
  }

  // Wrap floating-point variable declarations with "DeclareApeVar" or "DeclareApeVarInit".
  void process_float_var_decl(const MatchFinder::MatchResult& mresult) {
    process_var_decl(mresult, StringRef("float-decl"), std::string("Approx"));
  }

  // Wrap integer literals with "IntConst".
  void process_integer_literal(const MatchFinder::MatchResult& mresult) {
    const IntegerLiteral* intLit = mresult.Nodes.getNodeAs<IntegerLiteral>("int-lit");
    if (intLit == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(intLit->getSourceRange());
    SourceLocation ofs0(sr.getBegin());
    std::string text(get_text(sm, sr));
    Replacement rep(sm, ofs0, text.length(), "IntConst(" + text + ")");
    std::string fname(sm.getFilename(ofs0).str());
    if (replacements[fname].add(rep))
      llvm::errs() << "failed to perform replacement: " << rep.toString() << "\n";
  }

  // Wrap floating-point literals with "AConst".
  void process_float_literal(const MatchFinder::MatchResult& mresult) {
    const FloatingLiteral* floatLit = mresult.Nodes.getNodeAs<FloatingLiteral>("float-lit");
    if (floatLit == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(floatLit->getSourceRange());
    SourceLocation ofs0(sr.getBegin());
    std::string text(get_text(sm, sr));
    Replacement rep(sm, ofs0, text.length(), "Aconst(" + text + ")");
    std::string fname(sm.getFilename(ofs0).str());
    if (replacements[fname].add(rep))
      llvm::errs() << "failed to perform replacement: " << rep.toString() << "\n";
  }

  // Use Cast to cast types.
  void process_cast_expr(const MatchFinder::MatchResult& mresult) {
    // Determine the type of cast.
    const CastExpr* cast = mresult.Nodes.getNodeAs<CastExpr>("cast");
    if (cast == nullptr)
      return;
    std::string cast_str;
    switch (cast->getCastKind()) {
    case CK_FloatingToIntegral:
      cast_str = "Int";
      break;
    case CK_IntegralToFloating:
      cast_str = "Approx";
      break;
    case CK_IntegralToBoolean:
      cast_str = "Bool";
      break;
    default:
      return;
    }

    // Perform the cast.  The trick here is to wrap the value to be cast by
    // inserting text before and after it.  Otherwise, Clang would complain
    // about overlapping replacements.
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(cast->getSourceRange());
    SourceLocation ofs0(sr.getBegin());
    std::string text(get_text(sm, sr));
    std::string fname(sm.getFilename(ofs0).str());
    Replacement rep1(sm, ofs0, 0,
		     std::string("Cast(") + cast_str + ", ");
    if (replacements[fname].add(rep1))
      llvm::errs() << "failed to perform replacement: " << rep1.toString() << "\n";
    Replacement rep2(sm, ofs0.getLocWithOffset(text.length()), 0, ")");
    if (replacements[fname].add(rep2))
      llvm::errs() << "failed to perform replacement: " << rep2.toString() << "\n";
  }

public:
  // Store the set of replacements we were given to modify.
  explicit C_to_Nova(repl_map_t& repls) : replacements(repls) {}

  // Add a set of matchers to a finder.
  void add_matchers(MatchFinder& mfinder) {
    mfinder.addMatcher(integerLiteral(unless(hasAncestor(forStmt()))).bind("int-lit"), this);
    mfinder.addMatcher(floatLiteral(unless(hasAncestor(forStmt()))).bind("float-lit"), this);
    mfinder.addMatcher(varDecl(hasType(isInteger()),
                               unless(hasInitializer(expr().bind("rhs"))))
                       .bind("int-decl"), this);
    mfinder.addMatcher(varDecl(hasType(isInteger()),
                               hasInitializer(expr().bind("rhs")))
                       .bind("int-decl"), this);
    mfinder.addMatcher(varDecl(hasType(realFloatingPointType()),
                               unless(hasInitializer(expr().bind("rhs"))))
                       .bind("float-decl"), this);
    mfinder.addMatcher(varDecl(hasType(realFloatingPointType()),
                               hasInitializer(expr().bind("rhs")))
                       .bind("float-decl"), this);
    mfinder.addMatcher(castExpr(unless(hasAncestor(forStmt()))).bind("cast"), this);
  }

  // Process all of our matches.
  virtual void run(const MatchFinder::MatchResult& mresult) {
    process_integer_literal(mresult);
    process_float_literal(mresult);
    process_int_var_decl(mresult);
    process_float_var_decl(mresult);
    process_cast_expr(mresult);
  }
};

// As a user, I hate having to append "--" to the command line when running a
// Clang tool.  If we don't see a "--", append it ourself.  Also append
// "--language=c" in that case so Clang doesn't complain about our
// extensionless working file, "--std=c89" to prohibit newer features we don't
// expect to see, and "-fparse-all-comments" so we can parse magic comments.
bool append_options(int argc, const char **argv) {
  // Return true if the command line already contains a double-dash.
  for (int i = 1; i < argc; i++)
    if (std::string(argv[i]) == "--")
      return true;

  // No double dash: append one and restart the program.
  char **new_argv = new char *[argc + 5];
  for (int i = 0; i < argc; i++)
    new_argv[i] = strdup(argv[i]);
  new_argv[argc] = strdup("--");
  new_argv[argc + 1] = strdup("--language=c");
  new_argv[argc + 2] = strdup("--std=c89");
  new_argv[argc + 3] = strdup("-fparse-all-comments");
  new_argv[argc + 4] = nullptr;
  if (execvp(argv[0], new_argv) == -1)
    return false;
  return true; // We should never get here.
}

// Copy a file to a working file.  Return the name of the working file.
std::string copy_input_to_working(std::string iname) {
  // Create a working file with a random name.
  std::string tmpl_str(fs::temp_directory_path().append("c2nova-XXXXXX"));
  char* tmpl = strdup(tmpl_str.c_str());
  if (mkstemp(tmpl) == -1) {
    llvm::errs() << "failed to create a file from template " << tmpl << "\n";
    std::exit(1);
  }

  // Overwrite the working file with the input file.
  fs::path oname = fs::path(tmpl);
  fs::copy_file(iname, oname, fs::copy_options::overwrite_existing);
  return std::string(oname);
}

// Move the working file to the output file.
void move_working_to_output(fs::path iname, fs::path wname, fs::path oname) {
  // If the output name is empty, assign it a name derived from the input name.
  if (oname == "") {
    oname = fs::path(iname).replace_extension(".nova");
    if (oname == iname)
      oname += ".c";   // Don't implicitly overwrite the input file.
  }

  // Rename the working file to the output file.
  fs::rename(wname, oname);
}

int main(int argc, const char **argv) {
  // Append a "--" to the command line if none is already present.
  if (!append_options(argc, argv)) {
    llvm::errs() << "failed to restart " << argv[0] << "("
                 << std::strerror(errno) << ")\n";
    return 1;
  }

  // Parse the command line.
  auto opt_parser = CommonOptionsParser::create(argc, argv, c2n_opts, cl::Required);
  if (!opt_parser) {
    llvm::errs() << opt_parser.takeError();
    return 1;
  }

  // Copy the input file to a working file so we can modify it in place.
  std::string iname(opt_parser->getSourcePathList()[0]);
  std::string wname = copy_input_to_working(iname);
  std::vector<std::string> sources(1, wname);

  // Instantiate and prepare our Clang tool.
  RefactoringTool tool(opt_parser->getCompilations(), sources);
  C_to_Nova c2n(tool.getReplacements());
  MatchFinder mfinder;
  c2n.add_matchers(mfinder);

  // Run our Clang tool.
  int run_result = tool.runAndSave(newFrontendActionFactory(&mfinder).get());
  if (run_result != 0)
    return run_result;

  // Move the working file to the output file.
  move_working_to_output(iname, wname, fs::path(std::string(outfile)));
  return 0;
}
