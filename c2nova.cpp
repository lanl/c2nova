/***********************************
 * Translate C code to Nova macros *
 * By Scott Pakin <pakin@lanl.gov> *
 ***********************************/

#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Rewrite/Frontend/Rewriters.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Refactoring.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/CommandLine.h>
#include <cstdio>
#include <cstdlib>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

// Prepare --help to output some helpful information.
static llvm::cl::OptionCategory c2n_opts("cpp2nova options");
static cl::opt<std::string> outfile("o", cl::desc("specify output filename"), cl::value_desc("output.c"));
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// Match various operations and convert these to Nova.
class C_to_Nova : public clang::ast_matchers::MatchFinder::MatchCallback {
private:
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
    if (ofs0.isInvalid() || ofs1.isInvalid())
      return std::string("[c2nova:INVALID]"); // I don't know what causes this.
    const char* ptr0(sm.getCharacterData(ofs0));
    const char* ptr1(sm.getCharacterData(ofs1));
    return std::string(ptr0, ptr1);
  }

  // Return an identifier found at a given location.
  std::string get_ident(SourceManager& sm, SourceLocation id_begin) {
    LangOptions lopt;
    SourceLocation id_end(Lexer::getLocForEndOfToken(id_begin, 0, sm, lopt));
    if (id_begin.isInvalid() || id_end.isInvalid())
      return std::string("[c2nova:INVALID]"); // I don't know what causes this.
    const char* ptr0(sm.getCharacterData(id_begin));
    const char* ptr1(sm.getCharacterData(id_end));
    return std::string(ptr0, ptr1);
  }

  // Insert text before and after a given match.
  void insert_before_and_after(SourceManager& sm, SourceRange& sr,
                               const std::string& before_text,
                               const std::string& after_text) {
    SourceLocation ofs0(sr.getBegin());
    std::string text(get_text(sm, sr));
    prepare_rewriter(sm);
    rewriter->InsertTextBefore(ofs0, before_text);
    rewriter->InsertTextAfter(ofs0.getLocWithOffset(text.length()), after_text);
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
    prepare_rewriter(sm);
    if (rhs == nullptr)
      // No initializer.
      rewriter->ReplaceText(ofs0, text.length(), declare + "Init(" + var_name + ", " + nova_type + ")");
    else {
      // Initializer.
      const char* ptr0(sm.getCharacterData(ofs0));
      SourceRange rhs_sr(rhs->getSourceRange());
      SourceLocation rhs_ofs0(rhs_sr.getBegin());
      const char* rhs_ptr0(sm.getCharacterData(rhs_ofs0));
      rewriter->ReplaceText(ofs0, rhs_ptr0 - ptr0, declare + "Init(" + var_name + ", " + nova_type + ", ");
      SourceLocation ofs1(get_end_of_end(sm, sr));
      rewriter->InsertTextAfter(ofs1, ")");
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
    insert_before_and_after(sm, sr, "IntConst(", ")");
  }

  // Wrap floating-point literals with "AConst".
  void process_float_literal(const MatchFinder::MatchResult& mresult) {
    const FloatingLiteral* floatLit = mresult.Nodes.getNodeAs<FloatingLiteral>("float-lit");
    if (floatLit == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(floatLit->getSourceRange());
    insert_before_and_after(sm, sr, "AConst(", ")");
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

    // Perform the cast.
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(cast->getSourceRange());
    insert_before_and_after(sm, sr, std::string("Cast(") + cast_str + ", ", ")");
  }

  // Replace each unary operator with a corresponding Nova macro.
  void process_unary_operator(const MatchFinder::MatchResult& mresult) {
    // Map a Clang operator to a Nova macro name.
    const UnaryOperator* unop = mresult.Nodes.getNodeAs<UnaryOperator>("un-op");
    if (unop == nullptr)
      return;
    std::string mname;
    switch (unop->getOpcode()) {
    case UO_LNot:
      mname = "Not";
      break;
    case UO_Plus:
      mname = "";
      break;
    default:
      return;  // Unknown operator
    }

    // Remove the operator.
    SourceManager& sm(mresult.Context->getSourceManager());
    prepare_rewriter(sm);
    SourceLocation op_begin = unop->getOperatorLoc();
    SourceLocation arg_begin = unop->getSubExpr()->getBeginLoc();
    const char* ptr0(sm.getCharacterData(op_begin));
    const char* ptr1(sm.getCharacterData(arg_begin));
    size_t op_len = ptr1 - ptr0;
    rewriter->RemoveText(op_begin, op_len);

    // Wrap the operator's argument in a Nova macro plus parentheses.
    std::string before_text(mname + "(");
    std::string after_text(")");
    const Expr* arg = unop->getSubExpr();
    SourceRange arg_sr(arg->getBeginLoc(), arg->getEndLoc());
    insert_before_and_after(sm, arg_sr, before_text, after_text);
  }

  // Wrap each binary operator in a corresponding Nova macros.
  void process_binary_operator(const MatchFinder::MatchResult& mresult) {
    // Map a Clang operator to a Nova macro name.
    const BinaryOperator* binop = mresult.Nodes.getNodeAs<BinaryOperator>("bin-op");
    if (binop == nullptr)
      return;
    std::string mname;
    switch (binop->getOpcode()) {
      // Arithmetic operators (number op number -> number)
    case BO_Add:
    case BO_AddAssign:
      mname = "Add";
      break;
    case BO_Div:
    case BO_DivAssign:
      mname = "Div";
      break;
    case BO_Mul:
    case BO_MulAssign:
      mname = "Mul";
      break;
    case BO_Shl:
    case BO_ShlAssign:
      mname = "Asl";
      break;
    case BO_Shr:
    case BO_ShrAssign:
      mname = "Asr";
      break;
    case BO_Sub:
    case BO_SubAssign:
      mname = "Sub";
      break;

      // Logical operators (Boolean op Boolean -> Boolean)
    case BO_LAnd:
      mname = "And";
      break;
    case BO_LOr:
      mname = "Or";
      break;

      // Relational operators (number op number -> Boolean)
    case BO_EQ:
      mname = "Eq";
      break;
    case BO_NE:
      mname = "Ne";
      break;
    case BO_LT:
      mname = "Lt";
      break;
    case BO_LE:
      mname = "Le";
      break;
    case BO_GT:
      mname = "Gt";
      break;
    case BO_GE:
      mname = "Ge";
      break;

      // Other operators
    case BO_Assign:
      mname = "Set";
      break;
    default:
      return;  // Unknown operator
    }

    // Change the operator to a comma.
    SourceManager& sm(mresult.Context->getSourceManager());
    prepare_rewriter(sm);
    SourceLocation op_loc = binop->getOperatorLoc();
    StringRef op_text = binop->getOpcodeStr();
    rewriter->ReplaceText(op_loc, op_text.size(), ",");

    // Expand compound operators (e.g., "a *= b" becomes "Set(a, Mul(a, b))").
    std::string before_text(mname + "(");
    std::string after_text(")");
    if (binop->isCompoundAssignmentOp()) {
      Expr* lhs = binop->getLHS();
      SourceRange sr(lhs->getSourceRange());
      std::string lhs_text(get_text(sm, sr));
      before_text = std::string("Set(") + lhs_text + ", " + before_text;
      after_text += ")";
    }

    // Wrap the entire operation in a Nova macro.
    SourceRange sr(binop->getBeginLoc(), binop->getEndLoc());
    insert_before_and_after(sm, sr, before_text, after_text);
  }

  // Process function calls.  Currently, this merely renames sqrt() to Sqrt().
  void process_function_call(const MatchFinder::MatchResult& mresult) {
    const CallExpr* call = mresult.Nodes.getNodeAs<CallExpr>("call");
    if (call == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    const Expr* callee = call->getCallee();
    SourceRange sr(callee->getSourceRange());
    std::string callee_name = get_text(sm, sr);
    if (callee_name == "sqrt")
      rewriter->ReplaceText(sr.getBegin(), 4, "Sqrt");
  }

  // Initialize the rewriter if we haven't already.
  void prepare_rewriter(SourceManager& sm) {
    if (rewriter != nullptr)
      return;
    CompilerInstance comp_inst;
    rewriter = new Rewriter();
    rewriter->setSourceMgr(sm, comp_inst.getLangOpts());
  }

public:
  // Provide a do-nothing constructor.
  explicit C_to_Nova() {}

  // Return a modified source file as a string.
  std::string get_modifications() {
    if (rewriter == nullptr)
      return "";
    SourceManager& sm = rewriter->getSourceMgr();
    const RewriteBuffer *buf =
      rewriter->getRewriteBufferFor(sm.getMainFileID());
    return std::string(buf->begin(), buf->end());
  }

  // Rewriter to use for modifying the source code.
  Rewriter* rewriter = nullptr;

  // Add a set of matchers to a finder.
  void add_matchers(MatchFinder& mfinder) {
    // Variable declarations (int or float, with or without an initializer)
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

    // Int and float literals
    mfinder.addMatcher(integerLiteral().bind("int-lit"), this);
    mfinder.addMatcher(floatLiteral().bind("float-lit"), this);

    // Type cast
    mfinder.addMatcher(castExpr().bind("cast"), this);

    // Unary operator
    mfinder.addMatcher(unaryOperator().bind("un-op"), this);

    // Binary operator
    mfinder.addMatcher(binaryOperator().bind("bin-op"), this);

    // Function call
    mfinder.addMatcher(callExpr().bind("call"), this);
  }

  // Process all of our matches.
  virtual void run(const MatchFinder::MatchResult& mresult) {
    process_integer_literal(mresult);
    process_float_literal(mresult);
    process_int_var_decl(mresult);
    process_float_var_decl(mresult);
    process_cast_expr(mresult);
    process_unary_operator(mresult);
    process_binary_operator(mresult);
    process_function_call(mresult);
  }
};

// As a user, I hate having to append "--" to the command line when running a
// Clang tool.  If we don't see a "--", append it ourself.  Also append
// "--language=c" in that case so Clang doesn't complain about our
// extensionless working file, "--std=c89" to prohibit newer features we don't
// expect to see, and "-fparse-all-comments" so we can parse magic comments.
void append_options(int argc, const char** argv, int* new_argc, const char*** new_argv) {
  // Return the command line unmodified if it already contains a double-dash.
  for (int i = 1; i < argc; i++)
    if (std::string(argv[i]) == "--") {
      *new_argc = argc;
      *new_argv = argv;
      return;
    }

  // No double dash: append one.
  *new_argv = new const char* [argc + 5];
  for (int i = 0; i < argc; i++)
    (*new_argv)[i] = argv[i];
  (*new_argv)[argc] = "--";
  (*new_argv)[argc + 1] = "--language=c";
  (*new_argv)[argc + 2] = "--std=c89";
  (*new_argv)[argc + 3] = "-fparse-all-comments";
  (*new_argv)[argc + 4] = nullptr;
  *new_argc = argc + 4;
}

int main(int argc, const char **argv) {
  // Append a "--" to the command line if none is already present.
  int new_argc;
  const char** new_argv;
  append_options(argc, argv, &new_argc, &new_argv);

  // Parse the command line.
  auto opt_parser = CommonOptionsParser::create(new_argc, new_argv, c2n_opts, cl::Required);
  if (!opt_parser) {
    llvm::errs() << opt_parser.takeError();
    return 1;
  }

  // Instantiate and prepare our Clang tool.
  ClangTool tool(opt_parser->getCompilations(), opt_parser->getSourcePathList());
  C_to_Nova c2n;
  MatchFinder mfinder;
  c2n.add_matchers(mfinder);

  // Run our Clang tool.
  int run_result = tool.run(newFrontendActionFactory(&mfinder).get());
  if (run_result != 0)
    return run_result;

  // Output the modified source code.
  llvm::outs() << c2n.get_modifications();
  return 0;
}
