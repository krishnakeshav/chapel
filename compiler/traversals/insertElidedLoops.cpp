#include "insertElidedLoops.h"
#include "symtab.h"
#include "symscope.h"
#include "symbol.h"
#include "type.h"
#include "expr.h"
#include "stmt.h"
#include "stringutil.h"


class InsertElidedIndices : public Traversal {
 public:
  Symbol* indices;
  bool insert;
  InsertElidedIndices::InsertElidedIndices(Symbol* init_indices);
  void preProcessExpr(Expr* expr);
  void postProcessExpr(Expr* expr);
};


InsertElidedIndices::InsertElidedIndices(Symbol* init_indices) {
  indices = init_indices;
  insert = true;
}


void InsertElidedIndices::preProcessExpr(Expr* expr) {
  if (!insert) {
    return;
  }

  if (!dynamic_cast<ArrayType*>(expr->typeInfo())) {
    insert = false;
    return;
  }

  if (dynamic_cast<Variable*>(expr)) {
    Expr* index_exprs = NULL;
    for (Symbol* tmp = indices; tmp; tmp = nextLink(Symbol, tmp)) {
      index_exprs = appendLink(index_exprs, new Variable(tmp));
    }
    ArrayRef* array_ref = new ArrayRef(expr->copy(), index_exprs);
    expr->replace(array_ref);
  }
}


void InsertElidedIndices::postProcessExpr(Expr* expr) {
  insert = true;
}


void InsertElidedLoops::postProcessStmt(Stmt* stmt) {
  static int uid = 1;
  if (ExprStmt* expr_stmt = dynamic_cast<ExprStmt*>(stmt)) {
    if (AssignOp* assign = dynamic_cast<AssignOp*>(expr_stmt->expr)) {
      if (ArrayType* array_type = dynamic_cast<ArrayType*>(assign->left->typeInfo())) {
	Symbol* indices = NULL;
	for (int i = 0; i < array_type->domainType->numdims; i++) {
	  char* name = glomstrings(4, "_ind_", intstring(uid), "_", intstring(i));
	  indices = appendLink(indices, new Symbol(SYMBOL, name));
	}
	uid++;
	ForLoopStmt* loop = Symboltable::startForLoop(true, indices, array_type->domain->copy());
	loop = Symboltable::finishForLoop(loop, stmt->copy());
	stmt->replace(loop);
	TRAVERSE(loop->body, new InsertElidedIndices(loop->index), true);
      }
    }
  }
}
