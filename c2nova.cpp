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
  // Redirect source locations corresponding to macro expansions from the macro
  // definition to the macro use.
  SourceLocation fix_sl(SourceManager& sm, SourceLocation sl) {
    if (sl.isMacroID()) {
      CharSourceRange csl(sm.getImmediateExpansionRange(sl));
      return csl.getBegin();
    }
    else
      return sl;
  }

  // Apply fix_sl to both the beginning and ending locations in a source range.
  SourceRange fix_sr(SourceManager& sm, SourceRange sr) {
    return SourceRange(fix_sl(sm, sr.getBegin()), fix_sl(sm, sr.getEnd()));
  }

  // Apply fix_sl to two source locations, returning a source range.
  SourceRange fix_sr(SourceManager& sm, SourceLocation sl0, SourceLocation sl1) {
    return SourceRange(fix_sl(sm, sl0), fix_sl(sm, sl1));
  }

  // Return the end of the last token in a source range.
  SourceLocation get_end_of_end(SourceManager& sm, SourceRange sr) {
    SourceLocation end_tok_begin(sr.getEnd());
    LangOptions lopt;
    SourceLocation end_tok_end(Lexer::getLocForEndOfToken(end_tok_begin, 0, sm, lopt));
    return end_tok_end;
  }

  // Return the end of a token in a source location.
  SourceLocation get_end_of_end(SourceManager& sm, SourceLocation sl) {
    LangOptions lopt;
    SourceLocation end_tok_end(Lexer::getLocForEndOfToken(sl, 0, sm, lopt));
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

  // Return the text corresponding to a token at a source location.
  std::string get_text(SourceManager& sm, SourceLocation& sl) {
    SourceLocation sl_end(get_end_of_end(sm, sl));
    if (sl.isInvalid() || sl_end.isInvalid())
      return std::string("[c2nova:INVALID]"); // I don't know what causes this.
    const char* ptr0(sm.getCharacterData(sl));
    const char* ptr1(sm.getCharacterData(sl_end));
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

  // Describe a variable declaration.
  class VarDeclInfo {
  public:
    int dimens;            // -1 for invalid, 0 for scalar, 1 for vector, 2 for array
    std::string rows;      // Number of rows (2-D), vector length (1-D), or empty (scalar)
    std::string cols;      // Number of columns (2-D) or empty (1-D or scalar)
    std::string elt_type;  // Nova element type

    explicit VarDeclInfo(const clang::Type* var_type) {
      // Assume invalid
      dimens = -1;

      // Check if we were given a scalar.
      if (var_type->isIntegerType()) {
        dimens = 0;
        elt_type = "Int";
        return;
      }
      if (var_type->isFloatingType()) {
        dimens = 0;
        elt_type = "Approx";
        return;
      }

      // We were given a vector or possibly a vector of vectors.
      if (!var_type->isConstantArrayType())
        return;
      auto vec_type = cast<ConstantArrayType>(var_type);
      if (vec_type == nullptr)
        return;  // Unreachable?
      SmallString<25> nelt_str;
      vec_type->getSize().toString(nelt_str, 10, false, true);
      rows = std::string(nelt_str);
      const clang::Type& vec_elt_type = *vec_type->getElementType();
      if (vec_elt_type.isIntegerType()) {
        dimens = 1;
        elt_type = "Int";
        return;
      }
      if (vec_elt_type.isFloatingType()) {
        dimens = 1;
        elt_type = "Approx";
        return;
      }

      // We were given a vector of vectors or possibly a vector of vectors of
      // vectors (the latter being deemed invalid for translation to Nova).
      if (!vec_elt_type.isConstantArrayType())
        return;
      auto arr_type = cast<ConstantArrayType>(&vec_elt_type);
      if (arr_type == nullptr)
        return;  // Unreachable?
      nelt_str.clear();
      arr_type->getSize().toString(nelt_str, 10, false, true);
      cols = std::string(nelt_str);
      const clang::Type& arr_elt_type = *arr_type->getElementType();
      if (arr_elt_type.isIntegerType()) {
        dimens = 2;
        elt_type = "Int";
        return;
      }
      if (arr_elt_type.isFloatingType()) {
        dimens = 2;
        elt_type = "Approx";
        return;
      }
    }
  };

  // Wrap integer/float variable declarations with "DeclareApeVar" or
  // "DeclareApeVarInit", as appropriate.  This serves a helper function for
  // process_integer_decl() and process_float_decl().
  void process_var_decl(const MatchFinder::MatchResult& mresult) {
    // Extract the declaration in both raw and textual forms.
    const VarDecl* decl = mresult.Nodes.getNodeAs<VarDecl>("var-decl");
    if (decl == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(fix_sr(sm, decl->getSourceRange()));
    SourceLocation ofs0(sr.getBegin());
    std::string text(get_text(sm, sr));

    // Ensure we were given a data type we recognize.
    const clang::Type& var_type = *decl->getType();
    const VarDeclInfo type_info(&var_type);
    if (type_info.dimens < 0)
      return;

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

    // Extract the variable name.
    std::string var_name(get_ident(sm, decl->getLocation()));

    // Convert the C variable type to Nova.
    std::string nova_type = type_info.elt_type;    // "Int" or "Approx"
    if (type_info.dimens == 1)
      what = StringRef("MemVector");  // Vectors must reside in memory.
    else if (type_info.dimens == 2)
      what = StringRef("MemArray");   // Arrays must reside in memory.

    // Include extra arguments for vector/array sizes.
    std::string size_args;  // Vector size or empty
    if (type_info.dimens == 1)
      size_args = std::string(", ") + type_info.rows;
    else if (type_info.dimens == 2)
      size_args = std::string(", ") + type_info.rows + std::string(", ") + type_info.cols;

    // Generate a replacement either with or without an initializer.
    std::string declare(std::string("Declare") + where.str() + what.str());
    const Expr* rhs = decl->getInit();
    prepare_rewriter(sm);
    if (rhs == nullptr)
      // No initializer.
      if (type_info.dimens == 0)
        // Use nice rewriting for declaring scalars.
        rewriter->ReplaceText(sr, declare + '(' + var_name + ", " + nova_type + ')');
      else
        // Use an ugly hack to define vectors and arrays (commenting out the
        // code we don't know how to transform and inserting all-new code
        // before it).
        rewriter->InsertText(sr.getBegin(), declare + '(' + var_name + ", " + nova_type + size_args + ");  // ");
    else {
      // Initializer.
      SourceRange up_to_rhs(fix_sr(sm, ofs0, rhs->getBeginLoc().getLocWithOffset(-1)));
      rewriter->ReplaceText(up_to_rhs, declare + "Init(" + var_name + ", " + nova_type + ",");
      SourceLocation ofs1(get_end_of_end(sm, sr));
      rewriter->InsertTextAfter(ofs1, ")");
    }
  }

  // Wrap integer literals with "IntConst".
  void process_integer_literal(const MatchFinder::MatchResult& mresult) {
    const IntegerLiteral* intLit = mresult.Nodes.getNodeAs<IntegerLiteral>("int-lit");
    if (intLit == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(fix_sr(sm, intLit->getSourceRange()));
    insert_before_and_after(sm, sr, "IntConst(", ")");
  }

  // Wrap floating-point literals with "AConst".
  void process_float_literal(const MatchFinder::MatchResult& mresult) {
    const FloatingLiteral* floatLit = mresult.Nodes.getNodeAs<FloatingLiteral>("float-lit");
    if (floatLit == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(fix_sr(sm, floatLit->getSourceRange()));
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
    SourceRange sr(fix_sr(sm, cast->getSourceRange()));
    insert_before_and_after(sm, sr, std::string("Cast(") + cast_str + ", ", ")");
  }

  // Replace each unary operator with a corresponding Nova macro.
  void process_unary_operator(const MatchFinder::MatchResult& mresult) {
    // Map a Clang operator to a Nova macro name.
    const UnaryOperator* unop = mresult.Nodes.getNodeAs<UnaryOperator>("un-op");
    if (unop == nullptr)
      return;
    const Expr* arg = unop->getSubExpr();
    const clang::Type& arg_type = *arg->getType();
    std::string mname;
    std::string after_text(")");
    std::string inc_dec;  // Either "", "Sub" (for "--"), or "Add" (for "++")
    switch (unop->getOpcode()) {
    case UO_LNot:
      mname = "Not";
      break;
    case UO_Minus:
      // Nova doesn't appear to have a unary minus so we have to convert to a
      // binary operation.
      if (arg_type.isIntegerType()) {
        mname = "Sub(IntConst(0), ";
        after_text += ')';
      }
      else if (arg_type.isFloatingType()) {
        mname = "Sub(AConst(0.0), ";
        after_text += ')';
      }
      else
        return;
      break;
    case UO_Plus:
      mname = "";
      break;
    case UO_PostDec:
    case UO_PreDec:
      inc_dec = "Sub";
      after_text = ", IntConst(1))" + after_text;
      break;
    case UO_PostInc:
    case UO_PreInc:
      inc_dec = "Add";
      after_text = ", IntConst(1))" + after_text;
      break;
    default:
      return;  // Unknown operator
    }

    // Remove the operator.
    SourceManager& sm(mresult.Context->getSourceManager());
    prepare_rewriter(sm);
    SourceLocation op_begin = fix_sl(sm, unop->getOperatorLoc());
    std::string op_text(get_text(sm, op_begin));
    rewriter->RemoveText(op_begin, op_text.size());

    // Specially handle increment and decrement operators by converting them to
    // Set statements.
    SourceRange arg_sr(fix_sr(sm, arg->getSourceRange()));
    std::string before_text(mname + '(');
    if (inc_dec != "") {
      std::string arg_text(get_text(sm, arg_sr));
      before_text = std::string("Set(") + arg_text + ", " + inc_dec + '(';
    }

    // Wrap the operator's argument in a Nova macro plus parentheses.
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
    SourceLocation op_loc = fix_sl(sm, binop->getOperatorLoc());
    std::string op_text(get_text(sm, op_loc));
    rewriter->ReplaceText(op_loc, op_text.size(), ",");

    // Expand compound operators (e.g., "a *= b" becomes "Set(a, Mul(a, b))").
    std::string before_text(mname + '(');
    std::string after_text(")");
    if (binop->isCompoundAssignmentOp()) {
      Expr* lhs = binop->getLHS();
      SourceRange sr(fix_sr(sm, lhs->getSourceRange()));
      std::string lhs_text(get_text(sm, sr));
      before_text = std::string("Set(") + lhs_text + ", " + before_text;
      after_text += ')';
    }

    // Wrap the entire operation in a Nova macro.
    SourceRange sr(fix_sr(sm, binop->getSourceRange()));
    insert_before_and_after(sm, sr, before_text, after_text);
  }

  // Process array indexing.
  void process_array_index(const MatchFinder::MatchResult& mresult) {
    // Extract the expression's base and index.
    const ArraySubscriptExpr* aindex = mresult.Nodes.getNodeAs<ArraySubscriptExpr>("arr-idx");
    if (aindex == nullptr)
      return;
    const Expr* base = aindex->getBase();
    const Expr* idx = aindex->getIdx();

    // Wrap the base and index within a Nova macro invocation:
    // a[i] --> IndexVector(a, i).
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange base_sr(fix_sr(sm, base->getSourceRange()));
    SourceRange idx_sr(fix_sr(sm, idx->getSourceRange()));
    rewriter->InsertText(base->getBeginLoc(), "IndexVector(");
    SourceRange lbrack_sr(get_end_of_end(sm, base_sr),
                          idx_sr.getBegin().getLocWithOffset(-1));
    rewriter->ReplaceText(lbrack_sr, ", ");
    rewriter->ReplaceText(aindex->getRBracketLoc(), 1, ")");
  }

  // Process function calls.  Currently, this merely renames sqrt() to Sqrt().
  void process_function_call(const MatchFinder::MatchResult& mresult) {
    const CallExpr* call = mresult.Nodes.getNodeAs<CallExpr>("call");
    if (call == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    prepare_rewriter(sm);
    const Expr* callee = call->getCallee();
    SourceRange sr(fix_sr(sm, callee->getSourceRange()));
    std::string callee_name = get_text(sm, sr);
    if (callee_name == "sqrt")
      rewriter->ReplaceText(sr.getBegin(), 4, "Sqrt");
  }

  // Process if statements.  These are normally scheduled to execute on the
  // APEs, but if the if statement is invoked as a macro CU_IF (i.e., "#define
  // CU_IF if" and later "CU_IF (condition) statement"), the if statement will
  // be scheduled to execute on the CU.
  void process_if_statement(const MatchFinder::MatchResult& mresult) {
    // Define our replacement strings.
    const IfStmt* if_stmt = mresult.Nodes.getNodeAs<IfStmt>("if-stmt");
    if (if_stmt == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange if_sr(fix_sr(sm, if_stmt->getIfLoc(), if_stmt->getLParenLoc().getLocWithOffset(-1)));
    std::string if_text(get_text(sm, if_sr));
    std::string if_name("ApeIf");
    std::string else_name("ApeElse()");
    std::string fi_name("ApeFi()");
    if (if_text == "CU_IF") {
      if_name = "CUIf";
      else_name = "CUElse()";
      fi_name = "CUFi()";
    }

    // Replace "if".
    prepare_rewriter(sm);
    rewriter->ReplaceText(if_sr, if_name);

    // Replace "else", if present.
    SourceLocation else_begin(fix_sl(sm, if_stmt->getElseLoc()));
    if (else_begin.isValid()) {
      SourceLocation else_end(get_end_of_end(sm, else_begin));
      SourceRange else_sr(else_begin, else_end);
      rewriter->ReplaceText(else_sr, else_name);
    }

    // Insert "fi" at the end of the statement.
    SourceLocation end_loc(fix_sl(sm, if_stmt->getEndLoc()));
    SourceLocation fi_loc(get_end_of_end(sm, end_loc));
    rewriter->InsertTextAfterToken(fi_loc, fi_name);
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
    mfinder.addMatcher(varDecl().bind("var-decl"), this);

    // Int and float literals
    mfinder.addMatcher(integerLiteral().bind("int-lit"), this);
    mfinder.addMatcher(floatLiteral().bind("float-lit"), this);

    // Type cast
    mfinder.addMatcher(castExpr().bind("cast"), this);

    // Unary operator
    mfinder.addMatcher(unaryOperator().bind("un-op"), this);

    // Binary operator
    mfinder.addMatcher(binaryOperator().bind("bin-op"), this);

    // Array indexing
    mfinder.addMatcher(arraySubscriptExpr().bind("arr-idx"), this);

    // Function call
    mfinder.addMatcher(callExpr().bind("call"), this);

    // if statement
    mfinder.addMatcher(ifStmt().bind("if-stmt"), this);
  }

  // Process all of our matches.
  virtual void run(const MatchFinder::MatchResult& mresult) {
    process_integer_literal(mresult);
    process_float_literal(mresult);
    process_var_decl(mresult);
    process_cast_expr(mresult);
    process_unary_operator(mresult);
    process_binary_operator(mresult);
    process_array_index(mresult);
    process_function_call(mresult);
    process_if_statement(mresult);
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
