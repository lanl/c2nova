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
#include <queue>

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
  // Define how to modify text.
  typedef enum {
    mod_ins_before,
    mod_ins_after,
    mod_remove,
    mod_replace
  } pri_mod_t;

  // Represent a prioritized replacement.
  class PriRewrite {
  public:
    int priority;
    pri_mod_t operation;
    SourceRange range;
    int size;
    std::string text;

    explicit PriRewrite(int prio, pri_mod_t op, SourceRange sr, std::string txt) :
      priority(prio),
      operation(op),
      range(sr),
      size(0),
      text(txt) {
    }

    explicit PriRewrite(int prio, pri_mod_t op, SourceLocation sl, int sz, std::string txt="") :
      priority(prio),
      operation(op),
      size(sz),
      text(txt) {
      range = SourceRange(sl, sl);
    }

    bool operator<(const PriRewrite& other) const {
      // Perform smaller-valued priorities first.
      return priority > other.priority;
    }
  };

  // Maintain a priority queue of rewrites to perform.
  std::priority_queue<PriRewrite> rewrite_queue;

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
  static SourceLocation get_end_of_end(SourceManager& sm, SourceRange sr) {
    SourceLocation end_tok_begin(sr.getEnd());
    LangOptions lopt;
    SourceLocation end_tok_end(Lexer::getLocForEndOfToken(end_tok_begin, 0, sm, lopt));
    return end_tok_end;
  }

  // Return the end of a token in a source location.
  static SourceLocation get_end_of_end(SourceManager& sm, SourceLocation sl) {
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
  void insert_before_and_after(int priority,
                               SourceManager& sm, SourceRange& sr,
                               const std::string& before_text,
                               const std::string& after_text) {
    SourceLocation ofs0(sr.getBegin());
    std::string text(get_text(sm, sr));
    rewrite_queue.push(PriRewrite(priority, mod_ins_before, ofs0, before_text));
    rewrite_queue.push(PriRewrite(priority, mod_ins_after, ofs0.getLocWithOffset(text.length()), after_text));
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
      if (var_type->isBooleanType()) {
        dimens = 0;
        elt_type = "Bool";
        return;
      }
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
      if (vec_elt_type.isBooleanType()) {
        dimens = 1;
        elt_type = "Bool";
        return;
      }
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
      if (arr_elt_type.isBooleanType()) {
        dimens = 2;
        elt_type = "Bool";
        return;
      }
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

    // As a special case, top-level variables can only be declared, not defined.
    std::string declare(where.str() + what.str());
    if (decl->hasGlobalStorage())
      rewrite_queue.push(PriRewrite(70, mod_ins_before, sr,
                                    std::string("Declare(") + var_name + ");  // Within a function: "));
    else
      declare = std::string("Declare") + declare;

    // Generate a replacement either with or without an initializer.
    const Expr* rhs = decl->getInit();
    if (rhs == nullptr)
      // No initializer.
      if (type_info.dimens == 0)
        // Use nice rewriting for declaring scalars.
        rewrite_queue.push(PriRewrite(70, mod_replace, sr,
                                      declare + '(' + var_name + ", " + nova_type + ')'));
      else
        // Use an ugly hack to define vectors and arrays (commenting out the
        // code we don't know how to transform and inserting all-new code
        // before it).
        rewrite_queue.push(PriRewrite(70, mod_ins_before, sr,
                                      declare + '(' + var_name + ", " + nova_type + size_args + ");  // "));
    else {
      // Initializer.
      SourceRange up_to_rhs(fix_sr(sm, ofs0, rhs->getBeginLoc().getLocWithOffset(-1)));
      rewrite_queue.push(PriRewrite(70, mod_replace, up_to_rhs,
                                    declare + "Init(" + var_name + ", " + nova_type + ","));
      SourceLocation ofs1(get_end_of_end(sm, sr));
      rewrite_queue.push(PriRewrite(70, mod_ins_after, ofs1, ")"));
    }
  }

  // Wrap integer literals with "IntConst".
  void process_integer_literal(const MatchFinder::MatchResult& mresult) {
    const IntegerLiteral* intLit = mresult.Nodes.getNodeAs<IntegerLiteral>("int-lit");
    if (intLit == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(fix_sr(sm, intLit->getSourceRange()));
    insert_before_and_after(10, sm, sr, "IntConst(", ")");
  }

  // Wrap floating-point literals with "AConst".
  void process_float_literal(const MatchFinder::MatchResult& mresult) {
    const FloatingLiteral* floatLit = mresult.Nodes.getNodeAs<FloatingLiteral>("float-lit");
    if (floatLit == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(fix_sr(sm, floatLit->getSourceRange()));
    insert_before_and_after(10, sm, sr, "AConst(", ")");
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
    const ExplicitCastExpr* exp_cast = dyn_cast<ExplicitCastExpr>(cast);
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange sr(fix_sr(sm, cast->getSourceRange()));
    if (exp_cast == nullptr)
      // Implicit cast
      insert_before_and_after(60, sm, sr, std::string("Cast(") + cast_str + ", ", ")");
    else {
      // Explicit cast
      const Expr* sub_expr = cast->getSubExpr();
      SourceRange sub_sr(fix_sr(sm, sub_expr->getSourceRange()));
      SourceRange type_sr(sr.getBegin(), sub_sr.getBegin().getLocWithOffset(-1));
      rewrite_queue.push(PriRewrite(60, mod_replace, type_sr,
                                    std::string("Cast(") + cast_str + ", "));
      rewrite_queue.push(PriRewrite(60, mod_ins_after, get_end_of_end(sm, sr), ")"));
    }
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
    SourceLocation op_begin = fix_sl(sm, unop->getOperatorLoc());
    std::string op_text(get_text(sm, op_begin));
    rewrite_queue.push(PriRewrite(50, mod_remove, op_begin, op_text.size()));

    // Specially handle increment and decrement operators by converting them to
    // Set statements.
    SourceRange arg_sr(fix_sr(sm, arg->getSourceRange()));
    std::string before_text(mname + '(');
    if (inc_dec != "") {
      std::string arg_text(get_text(sm, arg_sr));
      before_text = std::string("Set(") + arg_text + ", " + inc_dec + '(';
    }

    // Wrap the operator's argument in a Nova macro plus parentheses.
    insert_before_and_after(50, sm, arg_sr, before_text, after_text);
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

      // Bitwise operators (Boolean op Boolean -> Boolean)
      // Technically, these work on integers but ignore all but the LSB.
    case BO_And:
      mname = "And";
      break;
    case BO_Or:
      mname = "Or";
      break;
    case BO_Xor:
      mname = "Xor";
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
    SourceLocation op_loc = fix_sl(sm, binop->getOperatorLoc());
    std::string op_text(get_text(sm, op_loc));
    rewrite_queue.push(PriRewrite(50, mod_replace, op_loc, op_text.size(), ","));

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
    insert_before_and_after(50, sm, sr, before_text, after_text);
  }

  // Process vector and array indexing.
  void process_array_index(const MatchFinder::MatchResult& mresult) {
    // Extract the expression's base and index.
    const ArraySubscriptExpr* aindex = mresult.Nodes.getNodeAs<ArraySubscriptExpr>("arr-idx");
    if (aindex == nullptr)
      return;
    const Expr* base = aindex->getBase();
    const Expr* idx = aindex->getIdx();

    // Determine if the base is itself an array-index expression (i.e., part of
    // a 2-D array access).
    const ArraySubscriptExpr* base_ase = nullptr;
    const Expr* base_base = nullptr;
    const Expr* base_idx = nullptr;
    const ImplicitCastExpr* base_ice = dyn_cast<ImplicitCastExpr>(base);
    if (base_ice != nullptr) {
      base_ase = dyn_cast<ArraySubscriptExpr>(base_ice->getSubExpr());
      if (base_ase != nullptr) {
        base_base = base_ase->getBase();
        base_idx = base_ase->getIdx();
      }
    }

    // Translate the expression from C to Nova.
    SourceManager& sm(mresult.Context->getSourceManager());
    SourceRange base_sr(fix_sr(sm, base->getSourceRange()));
    SourceRange idx_sr(fix_sr(sm, idx->getSourceRange()));
    SourceRange lbrack_sr(get_end_of_end(sm, base_sr),
                          idx_sr.getBegin().getLocWithOffset(-1));
    if (base_base == nullptr) {
      // a[i] --> IndexVector(a, i)
      rewrite_queue.push(PriRewrite(40, mod_ins_before, base->getBeginLoc(), "IndexVector("));
      rewrite_queue.push(PriRewrite(40, mod_replace, lbrack_sr, ", "));
      rewrite_queue.push(PriRewrite(40, mod_replace, aindex->getRBracketLoc(), 1, ")"));
    } else {
      // a[i][j] --> IndexArray(a, i, j)
      SourceRange base_base_sr(fix_sr(sm, base_base->getSourceRange()));
      SourceRange base_idx_sr(fix_sr(sm, base_idx->getSourceRange()));
      SourceRange base_lbrack_sr(get_end_of_end(sm, base_base_sr),
                                 base_idx_sr.getBegin().getLocWithOffset(-1));
      SourceRange inner_bracks_sr(base_ase->getRBracketLoc(),
                                  idx_sr.getBegin().getLocWithOffset(-1));
      rewrite_queue.push(PriRewrite(40, mod_ins_before, base->getBeginLoc(), "IndexArray("));
      rewrite_queue.push(PriRewrite(40, mod_replace, base_lbrack_sr, ", "));
      rewrite_queue.push(PriRewrite(40, mod_replace, inner_bracks_sr, ", "));
      rewrite_queue.push(PriRewrite(40, mod_replace,
                                    aindex->getRBracketLoc(), 1, ")"));
    }
  }

  // Process function calls.  Currently, this merely renames sqrt() to Sqrt().
  void process_function_call(const MatchFinder::MatchResult& mresult) {
    const CallExpr* call = mresult.Nodes.getNodeAs<CallExpr>("call");
    if (call == nullptr)
      return;
    SourceManager& sm(mresult.Context->getSourceManager());
    const Expr* callee = call->getCallee();
    SourceRange sr(fix_sr(sm, callee->getSourceRange()));
    std::string callee_name = get_text(sm, sr);
    if (callee_name == "sqrt")
      rewrite_queue.push(PriRewrite(50, mod_replace, sr.getBegin(), 4, "Sqrt"));
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
    std::string fi_name("\nApeFi()");
    if (if_text == "CU_IF") {
      if_name = "CUIf";
      else_name = "CUElse()";
      fi_name = "\nCUFi()";
    }

    // Replace "if".
    rewrite_queue.push(PriRewrite(80, mod_replace, if_sr, if_name));

    // Replace "else", if present.
    SourceLocation else_begin(fix_sl(sm, if_stmt->getElseLoc()));
    if (else_begin.isValid()) {
      SourceLocation else_end(get_end_of_end(sm, else_begin));
      SourceRange else_sr(else_begin, else_end);
      rewrite_queue.push(PriRewrite(80, mod_replace, else_sr, else_name));
    }

    // Insert "fi" at the end of the statement.
    SourceLocation end_loc(fix_sl(sm, if_stmt->getEndLoc()));
    SourceLocation fi_loc(get_end_of_end(sm, end_loc));
    rewrite_queue.push(PriRewrite(80, mod_ins_after, fi_loc, fi_name));
  }

public:
  // Provide a do-nothing constructor.
  explicit C_to_Nova() {}

  // Rewriter to use for modifying the source code.
  Rewriter* rewriter = nullptr;

  // On the first pass, we enqueue modifications in rewrite_queue.  On the
  // second pass, we apply these modifications in priority order.
  int pass = 1;

  // Apply all queued modifications in priority order.
  void apply_modifications(SourceManager* sm) {
    // Create a rewriter.
    CompilerInstance comp_inst;
    rewriter = new Rewriter();
    rewriter->setSourceMgr(*sm, comp_inst.getLangOpts());

    // Apply each modification in turn.
    for (; !rewrite_queue.empty(); rewrite_queue.pop()) {
      const PriRewrite& mod = rewrite_queue.top();
      switch (mod.operation) {
      case mod_ins_before:
        rewriter->InsertTextBefore(mod.range.getBegin(), mod.text);
        break;
      case mod_ins_after:
        rewriter->InsertTextAfter(mod.range.getBegin(), mod.text);
        break;
      case mod_remove:
        rewriter->RemoveText(mod.range.getBegin(), mod.size);
        break;
      case mod_replace:
        rewriter->ReplaceText(mod.range, mod.text);
        break;
      default:
        std::abort(); // Should never get here
        break;
      }
    }
  }

  // Return a modified source file as a string.
  std::string get_modifications() {
    if (rewriter == nullptr)
      return "";
    SourceManager& sm = rewriter->getSourceMgr();
    const RewriteBuffer *buf =
      rewriter->getRewriteBufferFor(sm.getMainFileID());
    return std::string(buf->begin(), buf->end());
  }

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

    // Array indexing (only the topmost, so not the "a[i]" in "a[i][j]").
    mfinder.addMatcher(arraySubscriptExpr(unless(hasAncestor(arraySubscriptExpr()))).bind("arr-idx"), this);

    // Function call
    mfinder.addMatcher(callExpr().bind("call"), this);

    // if statement
    mfinder.addMatcher(ifStmt().bind("if-stmt"), this);
  }

  // Process all of our matches.
  virtual void run(const MatchFinder::MatchResult& mresult) {
    switch (pass) {
    case 1:
      process_integer_literal(mresult);
      process_float_literal(mresult);
      process_cast_expr(mresult);
      process_unary_operator(mresult);
      process_binary_operator(mresult);
      process_array_index(mresult);
      process_function_call(mresult);
      process_var_decl(mresult);
      process_if_statement(mresult);
      break;

    case 2:
      apply_modifications(mresult.SourceManager);
      pass = 3;
      break;

    case 3:
      // Do-nothing pass
      break;

    default:
      std::abort();
      break;
    }
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

  // As a hack, run our tool again in "apply modifications" mode.  We need to
  // do this to gain access to a live SourceManager object.
  c2n.pass++;
  run_result = tool.run(newFrontendActionFactory(&mfinder).get());
  if (run_result != 0)
    return run_result;

  // Output the modified source code.
  llvm::outs() << c2n.get_modifications();
  return 0;
}
