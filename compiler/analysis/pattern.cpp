#include "geysa.h"
#include "pattern.h"
#include "pdb.h"
#include "if1.h"
#include "builtin.h"
#include "fun.h"
#include "pnode.h"
#include "fa.h"
#include "var.h"
#include "ast.h"

typedef Map<Fun *, Match *> MatchMap;

class Matcher {
 public:
  AVar *send;
  AVar *arg0;
  Partial_kind partial;
  Vec<Match *> *matches;
  MatchMap match_map;
  Vec<Fun *> *all_matches, *partial_matches;
  Vec<MPosition *> *all_positions;
  Map<MPosition *, int> mapped_positions;

  int pattern_match_sym(Sym *, MPosition *, Vec<Fun *> *, Vec<Fun *> &, int, Vec<Sym *> &);
  void build_positional_map(MPosition &, Vec<Fun *> **);
  void update_match_map(AVar *, CreationSet *, MPosition *, MPosition *, Vec<Fun *> &);
  void find_arg_matches(AVar *, MPosition &, MPosition *, Vec<Fun *> **, int, int);
  void find_all_matches(CreationSet *, Vec<AVar *> &, Vec<Fun *> **, MPosition &p, int);
  void record_all_positions();
  void find_best_cs_match(Vec<CreationSet *> &, MPosition &p, Vec<Fun *> &local_matches, 
			  Vec<Fun *> &result, int);
  void find_best_matches(Vec<AVar *> &, Vec<CreationSet *> &, Vec<Fun *> &, MPosition &, 
			 Vec<Fun *> &, int, int iarg = 0);
  int covers_formals(Fun *, Vec<CreationSet *> &, MPosition &, int);
  void default_arguments_and_partial_application(Vec<CreationSet *> &, MPosition &, Vec<Fun *> &, int);
  void set_filters(Vec<CreationSet *> &, MPosition &, Vec<Fun *> &);
  void cannonicalize_matches();

  Matcher(AVar *send, AVar *arg0, Partial_kind partial, Vec<Match *> *matches);
};

static ChainHash<MPosition *, MPositionHashFuns> cannonical_mposition;

void
MPosition::copy(MPosition &p) {
  pos.copy(p.pos);
  parent = p.parent;
}

MPosition::MPosition(MPosition &p) {
  copy(p);
}

MPosition *
cannonicalize_mposition(MPosition &p) {
  MPosition *cp = cannonical_mposition.get(&p);
  if (cp)
    return cp;
  cp = new MPosition;
  cp->pos.copy(p.pos);
  cannonical_mposition.put(cp);
  return cp;
}

MPosition *
cannonicalize_formal(MPosition &p, Match *m) {
  MPosition *cp = cannonical_mposition.get(&p);
  MPosition *acp = m->actual_to_formal_position.get(cp);
  cp = acp ? acp : cp;
  return cp;
}

// Given type, cp, partial_matches, return funs, using done
int 
Matcher::pattern_match_sym(Sym *type, MPosition *cp, Vec<Fun *> *local_matches,
			   Vec<Fun *> &funs, int out_of_position, Vec<Sym *> &done) 
{
  if (!out_of_position) {
    int found = 0;
    if (type->match_type) {
      Vec<Fun *> *fs = type->match_type->funs.get(cp);
      if (fs) {
	Vec<Fun *> ffs;
	if (local_matches) {
	  local_matches->set_intersection(*fs, ffs);
	  fs = &ffs;
	}
	found = 1;
	funs.set_union(*fs);
      }
    }
    forv_Sym(tt, type->dispatch_order)
      if (done.set_add(tt))
	found = pattern_match_sym(tt, cp, local_matches, funs, 0, done) || found;
    return found;
  } else {
    int found = 0;
    // for all the local possible matches, if the type is permitted at the argument
    Vec<Match *> matches;
    Vec<MPosition *> positions;
    forv_Fun(f, *local_matches) {
      Match *m = match_map.get(f);
      MPosition *m_cp = m->actual_to_formal_position.get(cp);
      Sym *m_sym = m->fun->arg_syms.get(m_cp);
      Sym *m_type = m_sym->must_specialize ? m_sym->must_specialize : sym_any;
      if (m_type->specializers.set_in(m_sym)) {
	funs.set_add(m->fun);
	found = 1;
      }
    }
    return found;
  }
}

Matcher::Matcher(AVar *asend, AVar *aarg0, Partial_kind apartial, Vec<Match *> *amatches) {
  send = asend;
  arg0 = aarg0;
  partial = apartial;
  matches = amatches;
  matches->clear();
  
  // use summary information from previous analysis iterations to restrict the search
  if (send->var->def->callees) {
    all_matches = new Vec<Fun *>(send->var->def->callees->funs);
    all_positions = &send->var->def->callees->arg_positions;
  } else {
    all_matches = send->var->def->code->ast->visible_functions();
    all_positions = 0;
  }
  if (all_matches)
    partial_matches = new Vec<Fun *>(*all_matches);
  else
    partial_matches = 0;
}

void
Matcher::update_match_map(AVar *a, CreationSet *cs, MPosition *cp, MPosition *positional_p,
			  Vec<Fun *> &new_matches) 
{
  if (positional_p)
    positional_p = cannonical_mposition.get(positional_p);
  forv_Fun(f, new_matches) if (f) {
    Match *m = match_map.get(f);
    if (!m) {
      m = new Match(f);
      match_map.put(f, m); 
    }
    MPosition *m_cp = !positional_p ? m->actual_to_formal_position.get(cp) : cp;
    m_cp = m_cp ? m_cp : cp;
    AType *t = m->all_filters.get(m_cp);
    if (!t) {
      t = new AType; 
      m->all_filters.put(m_cp, t);
      if (positional_p)
	m->named_to_positional.put(m_cp, positional_p);
    }
    t->set_add(cs);
  }
}

static int
named_position(CreationSet *cs, AVar *av, MPosition &p, MPosition *pp) {
  if (cs && cs->sym != sym_tuple && av->var->sym->name && 
      !av->var->sym->is_constant & !av->var->sym->is_symbol) {
    pp->copy(p);
    pp->set_top(av->var->sym->name);
    return 1;
  } else if ((!cs || cs->sym == sym_tuple) && av->var->sym->alt_name) {
    pp->copy(p);
    pp->set_top(av->var->sym->alt_name);
    return 1;
  }
  return 0;
}

void
Matcher::find_arg_matches(AVar *a, MPosition &p, MPosition *positional_p, 
			  Vec<Fun *> **local_matches, int recurse, int out_of_position)
{
  MPosition *cp = cannonical_mposition.get(&p); // cannonicalize against all known positions
  a->arg_of_send.set_add(send);
  if (cp && (!all_positions || all_positions->set_in(cp))) { // if known or in all for this pnode
    Vec<Fun *> funs;
    forv_CreationSet(cs, *a->out) if (cs) {
      Sym *sym = cs->sym;
      if (a->var->sym->aspect) {
	if (!a->var->sym->aspect->implementors.in(cs->sym))
	  continue;
	sym = a->var->sym->aspect;
      }
      Vec<Sym *> done;
      Vec<Fun *> new_funs, *new_funs_p = &new_funs;
      if (!pattern_match_sym(sym, cp, *local_matches, new_funs, out_of_position, done))
	continue;
      update_match_map(a, cs, cp, positional_p, new_funs);
      if (recurse && cs->vars.n)
	find_all_matches(cs, cs->vars, &new_funs_p, p, out_of_position);
      funs.set_union(new_funs);
    }
    if (!*local_matches) {
      *local_matches = new Vec<Fun *>(funs);
    } else {
      Vec<Fun *> saved_matches;
      saved_matches.move(**local_matches);
      forv_Fun(f, saved_matches) if (f) {
	if (funs.set_in(f))
	  (*local_matches)->set_add(f);
	else if (!f->args.get(cp)) // the function does not use this argument
	  (*local_matches)->set_add(f);
      }
      (*local_matches)->set_union(funs);
    }
  }
}

int
compar_last_position(const void *ai, const void *aj) {
  MPosition *i = *(MPosition**)ai;
  MPosition *j = *(MPosition**)aj;
  int ii = Position2int(i->pos.v[i->pos.n-1]);
  int jj = Position2int(j->pos.v[j->pos.n-1]);
  return (ii > jj) ? 1 : ((ii < jj) ? -1 : 0);
}

void
Matcher::build_positional_map(MPosition &p, Vec<Fun *> **funs) {
  if (*funs) {
    forv_Fun(f, **funs) if (f) {
      Match *m = match_map.get(f);
      Vec<MPosition *> named_actuals, named_actual_positions, positional_formals;
      // collect named argument positions at this level
      for (int i = 0; i < m->all_filters.n; i++) {
	MPosition *pp = m->all_filters.v[i].key;
	if (pp && pp->pos.n == p.pos.n + 1 &&
	    !memcmp(pp->pos.v, p.pos.v, p.pos.n * sizeof(void*))) 
	{
	  named_actuals.add(pp);
	  named_actual_positions.add(m->named_to_positional.get(pp));
	}
      }
      // collect formal positions not used by named arguments at this level
      for (int i = 0; i < f->arg_positions.n; i++) {
	MPosition *pp = f->arg_positions.v[i];
	if (pp->pos.n == p.pos.n + 1 &&
	    !memcmp(pp->pos.v, p.pos.v, p.pos.n * sizeof(void*)) &&
	    is_intPosition(pp->pos.v[pp->pos.n - 1]) &&
	    !named_actual_positions.set_in(pp))
	  positional_formals.add(pp);
      }
      // sort remaining formal argument positions and named actual positions
      qsort(positional_formals.v, positional_formals.n, sizeof(void*), compar_last_position);
      qsort(named_actual_positions.v, named_actual_positions.n, sizeof(void*), compar_last_position);
      // build a map from actual positionals to formal positionals
      MPosition new_p(p);
      new_p.push(1);
      int inamed_actual_position = 0;
      for (int i = 0; i < positional_formals.n; i++) {
	MPosition *new_cp = cannonicalize_mposition(new_p);
	while (inamed_actual_position < named_actual_positions.n &&
	       new_cp->pos.v[new_cp->pos.n - 1] == 
	       named_actual_positions.v[inamed_actual_position]->pos.v[new_cp->pos.n - 1])
	{
	  m->actual_to_formal_position.put(new_cp, named_actual_positions.v[inamed_actual_position]);
	  if (new_cp != named_actual_positions.v[inamed_actual_position])
	    mapped_positions.put(new_cp, 1);
	  inamed_actual_position++;
	  new_p.inc();
	}
	m->actual_to_formal_position.put(new_cp, positional_formals.v[i]);
	if (new_cp != positional_formals.v[i])
	  mapped_positions.put(new_cp, 1);
	new_p.inc();
      }
    }
  }
}

void
Matcher::find_all_matches(CreationSet *cs, Vec<AVar *> &args, Vec<Fun *> **funs, 
			  MPosition &p, int out_of_position) 
{
  // determine named arguments
  int some_named = 0;
  p.push(1);
  forv_AVar(av, args) {
    MPosition named_p;
    if (named_position(cs, av, p, &named_p)) {
      find_arg_matches(av, named_p, &p, funs, 0, out_of_position);
      some_named = 1;
    }
    p.inc();
  }
  // build positional map
  p.pop();
  if (some_named)
    build_positional_map(p, funs);
  // match all arguments
  p.push(1);
  forv_AVar(av, args) {
    int local_out_of_position = out_of_position;
    if (!local_out_of_position) {
      MPosition *cp = cannonicalize_mposition(p);
      if (mapped_positions.get(cp))
	local_out_of_position = 1;
    }
    find_arg_matches(av, p, 0, funs, 1, local_out_of_position);
    p.inc();
  }
  p.pop();
}

int
subsumes(Vec<Sym *> *xargs, Vec<Sym *> *yargs, int nargs) {
  int result = 0;
  for (int i = 0; i < nargs; i++) {
    Sym *xarg = xargs->v[i];
    Sym *yarg = yargs->v[i];
    Sym *xtype = xarg->must_specialize ? xarg->must_specialize : sym_any;
    Sym *ytype = yarg->must_specialize ? yarg->must_specialize : sym_any;
    if (xtype == ytype)
      continue;
    if (xtype->specializers.set_in(ytype)) {
      if (result >= 0)
	result = 1;
      else
	return 0;
    } else if (ytype->specializers.set_in(xtype)) {
      if (result <= 0)
	result = -1;
      else
	return 0;
    }
  }
  return result;
}

void
Matcher::set_filters(Vec<CreationSet *> &csargs, MPosition &p, Vec<Fun *> &matches) {
  forv_Fun(f, matches) {
    Match *m = match_map.get(f);
    p.push(1);
    for (int i = 0; i < csargs.n; i++) {
      MPosition *cp = cannonicalize_formal(p, m);
      AType *t = m->filters.get(cp);
      if (!t) {
	t = new AType; 
	m->filters.put(cp, t);
      }
      t->set_add(csargs.v[i]);
      p.inc();
    }
    p.pop();
  }
}

void 
Matcher::default_arguments_and_partial_application(
  Vec<CreationSet *> &csargs, MPosition &p, Vec<Fun *> &matches, int top_level)
{
  Vec<Fun *> new_matches;
  Vec<Fun *> complete;
  forv_Fun(f, matches) {
    Match *m = match_map.get(f);
    // collect all formals covered for f
    Vec<MPosition *> formals;
    p.push(1);
    for (int i = 0; i < csargs.n; i++) {
      MPosition *cp = cannonicalize_formal(p, m);
      formals.set_add(cp);
      p.inc();
    }
    p.pop();
    // compute defaults and partial applications
    Vec<MPosition *> defaults, needed;
    for (int i = 0; i < f->arg_positions.n; i++) {
      MPosition *pp = f->arg_positions.v[i];
      if (pp->pos.n == p.pos.n + 1 &&
	  !memcmp(pp->pos.v, p.pos.v, p.pos.n * sizeof(void*)) &&
	  is_intPosition(pp->pos.v[pp->pos.n - 1])) 
      {
	if (formals.set_in(pp))
	  continue; // provided
	if (f->default_args.get(pp))
	  defaults.add(pp);
	needed.add(pp);
      }
    }
    if (needed.n) {
      if (top_level) {
	m->partial = 1;
	new_matches.add(f);
      }
      continue; // doesn't match
    }
    if (defaults.n) {
      // build new wrapper and put into new_matches
    }
    complete.add(f);
    new_matches.add(f);
  }
  if (complete.n > 1 && top_level)
    type_violation(ATypeViolation_DISPATCH_AMBIGUITY, arg0, arg0->out, send, 
		   new Vec<Fun *>(complete));
  matches.move(new_matches);
}

int
Matcher::covers_formals(Fun *f, Vec<CreationSet *> &csargs, MPosition &p, int top_level) {
  if (top_level && partial != Partial_NEVER)
    return 1;
  Match *m = match_map.get(f);
  Vec<MPosition *> formals;
  p.push(1);
  for (int i = 0; i < csargs.n; i++) {
    MPosition *cp = cannonicalize_formal(p, m);
    formals.set_add(cp);
    p.inc();
  }
  p.pop();
  for (int i = 0; i < f->arg_positions.n; i++) {
    MPosition *pp = f->arg_positions.v[i];
    if (pp->pos.n == p.pos.n + 1 &&
	!memcmp(pp->pos.v, p.pos.v, p.pos.n * sizeof(void*)) &&
	is_intPosition(pp->pos.v[pp->pos.n - 1])) 
    {
      if (formals.set_in(pp) || f->default_args.get(pp))
	continue;
      return 0;
    }
  }
  return 1;
}

void
Matcher::find_best_cs_match(Vec<CreationSet *> &csargs, MPosition &p, 
			    Vec<Fun *> &local_matches, 
			    Vec<Fun *> &result, int top_level)
{
  Vec<Match *> applicable;
  MPosition *local_p = cannonical_mposition.get(&p);
  // collect the matches which cover the argument CreationSets
  forv_Fun(f, local_matches) if (f) {
    Match *m = match_map.get(f);
    p.push(1);
    for (int i = 0; i < csargs.n; i++) {
      MPosition *cp = cannonical_mposition.get(&p);
      AType *t = m->all_filters.get(cp);
      if (!t || !t->set_in(csargs.v[i]))
	goto LnextFun;
      p.inc();
    }
    if (covers_formals(f, csargs, p, top_level))
      applicable.set_add(m);
  LnextFun:
    p.pop();
  }
  if (!applicable.n)
    return;
  Vec<Match *> unsubsumed, subsumed;
  // eliminate those which are subsumed by some other function
  for (int i = 0; i < applicable.n; i++) if (applicable.v[i]) {
    if (subsumed.set_in(applicable.v[i]))
      continue;
    for (int j = i + 1; j < applicable.n; j++) if (applicable.v[j]) {
      Vec<Sym *> *xargs, *yargs;
      if (top_level) {
	xargs = &applicable.v[i]->fun->sym->has;
	yargs = &applicable.v[j]->fun->sym->has;
      } else {
	xargs = &applicable.v[i]->fun->arg_syms.get(local_p)->has;
	yargs = &applicable.v[j]->fun->arg_syms.get(local_p)->has;
      }
      switch (subsumes(xargs, yargs, csargs.n)) {
	case -1: subsumed.set_add(applicable.v[j]); break;
	case 0: break;
	case 1: goto LnextApplicable;
      }
    }
    unsubsumed.add(applicable.v[i]);
  LnextApplicable:;
  }
  // for those remaining, check for ambiguities
  Vec<Fun *> matches;
  Vec<Fun *> similar;
  Fun *x = unsubsumed.v[0]->fun;	// all unambiguous results must be similar
  similar.add(x);
  for (int j = 1; j < unsubsumed.n; j++) {
    Fun *y = unsubsumed.v[j]->fun;
    for (int i = 0; i < csargs.n; i++) {
      Sym *xarg, *yarg;
      if (top_level) {
	xarg = x->sym->has.v[i];
	yarg = y->sym->has.v[i];
      } else {
	xarg = x->arg_syms.get(local_p)->has.v[i];
	yarg = y->arg_syms.get(local_p)->has.v[i];
      }
      Sym *xtype = xarg->must_specialize ? xarg->must_specialize : sym_any;
      Sym *ytype = yarg->must_specialize ? yarg->must_specialize : sym_any;
      if (xtype != ytype || xarg->is_pattern != yarg->is_pattern) {
	matches.set_add(x);
	matches.set_add(y);
	goto LnextUnsubsumed;
      }
    }
    similar.add(y);
  LnextUnsubsumed:;
  }
  // for similar functions recurse for patterns to check for subsumption and ambiguities
  if (similar.n) {
    p.push(1);
    for (int i = 0; i < csargs.n; i++) {
      Sym *arg = similar.v[0]->arg_syms.get(cannonical_mposition.get(&p));
      if (arg->is_pattern) {
	Vec<CreationSet *> local_csargs;
	Vec<Fun *> local_result;
	find_best_matches(csargs.v[i]->vars, local_csargs, similar, p, local_result, 0);
	similar.copy(local_result);
      }
      p.inc();
    }
    p.pop();
  }
  // recurse to set filters for matches
  forv_Fun(f, matches) if (f) {
    p.push(1);
    for (int i = 0; i < csargs.n; i++) {
      Sym *arg = similar.v[0]->arg_syms.get(cannonical_mposition.get(&p));
      if (arg->is_pattern) {
	Vec<CreationSet *> local_csargs;
	Vec<Fun *> local_result, match;
	match.add(f);
	find_best_matches(csargs.v[i]->vars, local_csargs, match, p, local_result, 0);
      }
      p.inc();
    }
    p.pop();
  }
  // handle defaults and set filters
  matches.set_union(similar);
  matches.set_to_vec();
  default_arguments_and_partial_application(csargs, p, matches, top_level);
  set_filters(csargs, p, matches);
  result.set_union(matches);
}

void
Matcher::find_best_matches(Vec<AVar *> &args, Vec<CreationSet *> &csargs, 
			   Vec<Fun *> &matches, MPosition &p, 
			   Vec<Fun *> &result, 
			   int top_level,
			   int iarg) 
{
  if (iarg >= args.n)
    find_best_cs_match(csargs, p, *partial_matches, result, top_level);
  else {
    csargs.fill(iarg + 1);
    forv_CreationSet(cs, *args.v[iarg]->out) if (cs) {
      csargs.v[iarg] = cs;
      find_best_matches(args, csargs, matches, p, result, top_level, iarg + 1);
    }
  }
}

void
Matcher::record_all_positions() {
  if (!all_positions) {
    all_positions = new Vec<MPosition *>;
    forv_Fun(f, *partial_matches) if (f)
      all_positions->set_union(f->arg_positions);
  }
}

void
Matcher::cannonicalize_matches() {
  forv_Fun(f, *partial_matches) if (f) {
    Match *m = match_map.get(f);
    for (int i = 0; i < m->filters.n; i++)
      if (m->filters.v[i].key)
	m->filters.v[i].value = make_AType(*m->filters.v[i].value);
    matches->add(m);
    // build_wrappers
    assert(!m->default_args.n && !m->generic_args.n && !m->pointwise_args.n);
  }
}

// main dispatch entry point - given a vector of arguments return a vector of matches
int
pattern_match(Vec<AVar *> &args, AVar *send, Partial_kind partial, Vec<Match *> *matches) {
  Matcher matcher(send, args.v[0], partial, matches);
  {
    MPosition p;
    matcher.find_all_matches(0, args, &matcher.partial_matches, p, 0);
    if (!matcher.partial_matches || !matcher.partial_matches->n)
      return 0;
  }
  matcher.record_all_positions();
  {
    MPosition p;
    Vec<CreationSet *> csargs;
    Vec<Fun *> result;
    matcher.find_best_matches(args, csargs, *matcher.partial_matches, p, result, 1);
    matcher.partial_matches->copy(result);
    if (!matcher.partial_matches->n)
      return 0;
  }
  matcher.cannonicalize_matches();
  return matches->n;
}

static void
insert_fun(FA *fa, Fun *f, Sym *arg, Sym *s, MPosition &p) {
  MPosition *cpnum = cannonicalize_mposition(p), *cpname = 0;
  if (arg->name && !arg->is_constant && !arg->is_symbol) {
    MPosition pp(p);
    pp.set_top(arg->name);
    cpname = cannonicalize_mposition(pp);
  }
  for (MPosition *cp = cpnum; cp; cp = cpname, cpname = 0) { 
    if (fa->patterns->types_set.set_add(s))
      fa->patterns->types.add(s);
    if (!s->match_type) {
      s->match_type = new MType;
      fa->patterns->mtypes.add(s->match_type);
    }
    Vec<Fun *> *funs = s->match_type->funs.get(cp);
    if (!funs) {
      funs = new Vec<Fun *>;
      s->match_type->funs.put(cp, funs);
    }
    funs->set_add(f);
  }
}

static void
build_arg(FA *fa, Fun *f, Sym *a, MPosition &p) {
  if ((a->is_symbol || (a->type && a->type->is_symbol))) {
    Sym *sel = a->is_symbol ? a : a->type;
    insert_fun(fa, f, a, sel, p);
  } else
    insert_fun(fa, f, a, a->must_specialize ? a->must_specialize : sym_any, p);
  if (a->is_pattern) {
    p.push(1);
    forv_Sym(aa, a->has)
      build_arg(fa, f, aa, p);
    p.pop();
  }
  p.inc();
}

static void
build(FA *fa) {
  forv_Fun(f, fa->pdb->funs) {
    MPosition p;
    p.push(1);
    insert_fun(fa, f, f->sym, f->sym, p);
    forv_Sym(a, f->sym->has)
      build_arg(fa, f, a, p);
  }
}

void
build_patterns(FA *fa) {
  fa->patterns = new Patterns;
  build(fa);
}

// build a Fun::arg_position - positional and named argument Positions for an arg or pattern
static void
build_arg_position(Fun *f, Sym *a, MPosition &p, MPosition *parent = 0) {
  MPosition *cpnum = cannonicalize_mposition(p), *cpname = 0;
  if (a->name && !a->is_constant && !a->is_symbol) {
    MPosition pp(p);
    pp.set_top(a->name);
    cpname = cannonicalize_mposition(pp);
  }
  for (MPosition *cp = cpnum; cp; cp = cpname, cpname = 0) { 
    cp->parent = parent;
    f->arg_positions.add(cp);
    if (!a->var)
      a->var = new Var(a);
    f->arg_syms.put(cp, a);
    if (a->is_pattern) {
      MPosition pp(p);
      pp.push(1);
      forv_Sym(aa, a->has) {
	build_arg_position(f, aa, pp, cp);
	pp.inc();
      }
      pp.pop();
    }
  }
}

// build Fun::arg_positions - vec of positional and named argument Positions
// build Fun::args - map from Position's to Var's
void
build_arg_positions(FA *fa) {
  forv_Fun(f, fa->pdb->funs) {
    MPosition p;
    p.push(1);
    forv_Sym(a, f->sym->has) {
      build_arg_position(f, a, p);
      p.inc();
    }
    forv_MPosition(p, f->arg_positions) {
      Sym *s = f->arg_syms.get(p);
      f->args.put(p, s->var);
    }
    f->rets.add(f->sym->ret->var);
  }
}
