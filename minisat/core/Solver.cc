/***************************************************************************************[Solver.cc]
 Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson
 Copyright (c) 2007-2010, Niklas Sorensson

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
 OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 **************************************************************************************************/

#include <cmath>

#include "minisat/mtl/Sort.h"
#include "minisat/core/Solver.h"

/*AB*/
#include "minisat/mtl/Vec.h"
#include "minisat/mtl/Heap.h"
#include "minisat/mtl/Alg.h"
#include "minisat/utils/Options.h"

#include <vector>
#include <iostream>
#include <sstream>
#include <cstdarg>
#include <algorithm>

#include "utils/Utils.hpp"
#include "utils/Print.hpp"
#include "theorysolvers/PCSolver.hpp"
#include "external/TerminationManagement.hpp"

using namespace std;
using namespace MinisatID;
/*AE*/

using namespace Minisat;

void reportf(const char* format, ...) {
	fflush(stdout);
	va_list args;
	va_start(args, format);
	fprintf(stderr, format, args);
	fflush(stderr);
}

//=================================================================================================
// Options:

static const char* _cat = "CORE";

static DoubleOption opt_var_decay(_cat, "var-decay", "The variable activity decay factor", 0.95, DoubleRange(0, false, 1, false));
static DoubleOption opt_clause_decay(_cat, "cla-decay", "The clause activity decay factor", 0.999, DoubleRange(0, false, 1, false));
static DoubleOption opt_random_var_freq(_cat, "rnd-freq", "The frequency with which the decision heuristic tries to choose a random variable", 0,
		DoubleRange(0, true, 1, true));
static DoubleOption opt_random_seed(_cat, "rnd-seed", "Used by the random variable selection", 91648253, DoubleRange(0, false, HUGE_VAL, false));
static IntOption opt_ccmin_mode(_cat, "ccmin-mode", "Controls conflict clause minimization (0=none, 1=basic, 2=deep)", 2, IntRange(0, 2));
static IntOption opt_phase_saving(_cat, "phase-saving", "Controls the level of phase saving (0=none, 1=limited, 2=full)", 2, IntRange(0, 2));
static BoolOption opt_rnd_init_act(_cat, "rnd-init", "Randomize the initial activity", false);
static BoolOption opt_luby_restart(_cat, "luby", "Use the Luby restart sequence", true);
static IntOption opt_restart_first(_cat, "rfirst", "The base restart interval", 100, IntRange(1, INT32_MAX));
static DoubleOption opt_restart_inc(_cat, "rinc", "Restart interval increase factor", 2, DoubleRange(1, false, HUGE_VAL, false));
static DoubleOption opt_garbage_frac(_cat, "gc-frac", "The fraction of wasted memory allowed before a garbage collection is triggered", 0.20,
		DoubleRange(0, false, HUGE_VAL, false));

//=================================================================================================
// Constructor/Destructor:

Solver::Solver(/*AB*/PCSolver* s/*AE*/)
		: /*A*/Propagator(s),
			/*A*/fullassignment(false),
			// Parameters (user settable):
			//
			/*A*/verbosity(getPCSolver().verbosity()),
			var_decay(opt_var_decay), clause_decay(opt_clause_decay), random_var_freq(opt_random_var_freq), random_seed(opt_random_seed),
			luby_restart(opt_luby_restart), ccmin_mode(opt_ccmin_mode), phase_saving(opt_phase_saving), rnd_pol(false), rnd_init_act(opt_rnd_init_act),
			garbage_frac(opt_garbage_frac), restart_first(opt_restart_first), restart_inc(opt_restart_inc)

			// Parameters (the rest):
			//
					,
			learntsize_factor((double) 1 / (double) 3), learntsize_inc(1.1)

			// Parameters (experimental):
			//
					,
			learntsize_adjust_start_confl(100), learntsize_adjust_inc(1.5)

			/*A*/,
			usecustomheur(false)
			/*A*/,
			customheurfreq(0.75)

			// Statistics: (formerly in 'SolverStats')
			//
					,
			solves(0), starts(0), decisions(0), rnd_decisions(0), propagations(0), conflicts(0), dec_vars(0), clauses_literals(0), learnts_literals(0),
			max_literals(0), tot_literals(0), ok(true), cla_inc(1), var_inc(1), watches(WatcherDeleted(ca)), qhead(0), simpDB_assigns(-1), simpDB_props(0),
			order_heap(VarOrderLt(activity)), progress_estimate(0), remove_satisfied(true)
			// Resource constraints:
			//
					,
			conflict_budget(-1), propagation_budget(-1), asynch_interrupt(false) {
	/*AB*/
	getPCSolver().accept(this, EV_PROPAGATE);
	getPCSolver().accept(this, EV_PRINTSTATS);
	getPCSolver().acceptFinishParsing(this, false);
	/*AE*/
}

Solver::~Solver() {
}

void Solver::setDecidable(Var v, bool decide) // NOTE: no-op if already a decision var!
{
	bool newdecidable = decide && !decision[v];
	if( newdecidable){
		dec_vars++;
	} else if (!decide &&  decision[v]) dec_vars--;

	if(verbosity>10){
		if(decide){
			clog <<">>> Making " <<mkPosLit(v) <<" decidable.\n";
		}else if(not decide && decision[v]){
			clog <<">>> Making decidable " <<mkPosLit(v) <<" undecidable.\n";
		}
	}

    decision[v] = decide;
    insertVarOrder(v);

    if(newdecidable){
    	getPCSolver().notifyBecameDecidable(v);
    }
}

//=================================================================================================
// Minor methods:

// Creates a new SAT variable in the solver. If 'decision' is cleared, variable will not be
// used as a decision variable (NOTE! This has effects on the meaning of a SATISFIABLE result).
//
Var Solver::newVar(lbool upol, bool dvar) {
	int v = nVars();
	watches.init(mkLit(v, false));
	watches.init(mkLit(v, true));
	assigns.push(l_Undef);
	vardata.push(mkVarData(CRef_Undef, 0));
	//activity .push(0);
	activity.push(rnd_init_act ? drand(random_seed) * 0.00001 : 0);
	seen.push(0);
	polarity.push(true);
	user_pol.push(upol);
	decision.push();
	trail.capacity(v + 1);
	getPCSolver().notifyVarAdded(); // NOTE: important before setting decidability
	setDecidable(v, dvar);
	return v;
}

inline void Solver::createNewDecisionLevel() {
	trail_lim.push(trail.size());
	/*A*/getPCSolver().newDecisionLevel();
}

/*AB*/
void Solver::finishParsing(bool& present) {
	present = true;
	if(not simplify()){
		getPCSolver().notifyUnsat();
	}
}

std::vector<Lit> Solver::getDecisions() const {
	std::vector<Lit> v;
	for (int i = 0; i < trail_lim.size(); i++) {
		v.push_back(trail[trail_lim[i]]);
	}
	return v;
}

void Solver::addLearnedClause(CRef rc) {
	Clause& c = ca[rc];
	if (c.size() > 1) {
		addToClauses(rc, true);
		attachClause(rc);
		claBumpActivity(c);
		if (verbosity >= 3) {
			reportf("Learned clause added: ");
			printClause(rc);
			reportf("\n");
		}
	} else {
		assert(c.size()==1);
		cancelUntil(0);
		vec<Lit> ps;
		ps.push(c[0]);
		addClause(ps);
	}
}

bool Solver::totalModelFound() {
	Var v = var_Undef;
	while (v == var_Undef || assigns[v] != l_Undef || !decision[v]) {
		if (v != var_Undef)
			order_heap.removeMin();
		if (order_heap.empty()) {
			v = var_Undef;
			break;
		} else
			v = order_heap[0];
	}
	return v == var_Undef;
}

struct permute{
	int newposition;
	Lit value;
	permute(int newpos, Lit value): newposition(newpos), value(value){
	}
};

struct lessthan_permute{
	bool operator() (const permute& lhs, const permute& rhs){
		return lhs.newposition<rhs.newposition;
	}
};

// NOTE: do not reimplement as sort with a random comparison operator, comparison should be CONSISTENT on consecutive calls!
void permuteRandomly(vec<Lit>& lits){
	vector<permute> newpositions;
	for(int i=0; i<lits.size(); ++i){
		newpositions.push_back(permute(rand(), lits[i]));
	}
	std::sort(newpositions.begin(), newpositions.end(), lessthan_permute());
	for(int i=0; i<lits.size(); ++i){
		lits[i] = newpositions[i].value;
	}
}

bool Solver::addBinaryOrLargerClause(vec<Lit>& ps, CRef& newclause) {
	assert(decisionLevel()==0); // TODO can also relax this here

	if (!ok){
		return false;
	}

	sort(ps); // NOTE: remove duplicates
	assert(ps.size()>1);

	permuteRandomly(ps); // NOTE: reduce dependency on grounding and literal introduction mechanics (certainly for lazy grounding)

	CRef cr = ca.alloc(ps, false);
	addToClauses(cr, false);
	attachClause(cr);
	newclause = cr;

	return true;
}
/*AE*/



bool Solver::addClause_(vec<Lit>& ps) {
	if (!ok){
		return false;
	}

	if(decisionLevel()>0){
		int nonfalsecount = 0;
		for(int i=0; i<ps.size() && nonfalsecount<2; ++i){
			if(not isFalse(ps[i])){
				nonfalsecount++;
			}
		}
		if(nonfalsecount<2){
			cancelUntil(0);
			return addClause_(ps);
		}
	}

	sort(ps); // NOTE: remove duplicates

	if(decisionLevel()==0){
		// Check satisfaction and remove false literals
		Lit p;
		int i, j;
		for (i = j = 0, p = lit_Undef; i < ps.size(); i++){
			if (value(ps[i]) == l_True || ps[i] == ~p){
				return true;
			}else if (value(ps[i]) != l_False && ps[i] != p){
				ps[j++] = p = ps[i];
			}
		}
		ps.shrink(i - j);
	}

	// NOTE: sort randomly to reduce dependency on grounding and literal introduction mechanics (certainly for lazy grounding)
	permuteRandomly(ps);

	if (ps.size() == 0) {
		return ok = false;
	} else if (ps.size() == 1) {
		assert(decisionLevel()==0);
		uncheckedEnqueue(ps[0]);
		return ok = (propagate() == CRef_Undef);
	} else {
		if(decisionLevel()>0){
			for(int i=0; i<ps.size(); ++i){
				if(not isFalse(ps[i])){
					auto temp = ps[i];
					ps[i] = ps[1];
					ps[1] = temp;
					break;
				}
			}
		}
		CRef cr = ca.alloc(ps, false);
		addToClauses(cr, false);
		attachClause(cr);
	}

	return true;
}

/*AB*/
void Solver::addToClauses(CRef cr, bool learnt) {
	getPCSolver().notifyClauseAdded(cr);
	if (learnt) {
		learnts.push(cr);
	} else {
		clauses.push(cr);
	}
}

/**
 * Checks whether at least one watch is a decision variable.
 * If not, it randomly chooses one and makes it a decision variable
 * This guarantees that when all decision vars have been chosen, all clauses are certainly satisfied
 *
 * complexity: O(1)
 */
void Solver::checkDecisionVars(const Clause& c) {
	assert(not isFalse(c[0]) || not isFalse(c[1]));
	if(isFalse(c[0])){
		setDecidable(var(c[1]), true);
	}else if(isFalse(c[1])){
		setDecidable(var(c[0]), true);
	}else if (not isDecisionVar(var(c[0])) && not isDecisionVar(var(c[1]))) {
		int choice = irand(random_seed, 2);
		assert(choice==0 || choice==1);
		setDecidable(var(c[choice]), true);
	}
	assert((not isFalse(c[0]) && isDecisionVar(var(c[0]))) || (not isFalse(c[1]) && isDecisionVar(var(c[1]))));
}
/*AE*/

void Solver::attachClause(CRef cr) {
	const Clause& c = ca[cr];
	assert(c.size() > 1);
	if(not c.learnt()){
		assert(not isFalse(c[1]) || not isFalse(c[0]));
	}
	watches[~c[0]].push(Watcher(cr, c[1]));
	watches[~c[1]].push(Watcher(cr, c[0]));
	if (c.learnt())
		learnts_literals += c.size();
	else
		clauses_literals += c.size();

	/*AB*/
	if(not c.learnt() || (not isFalse(c[1]) || not isFalse(c[0]))){
		checkDecisionVars(c);
	}
	//printClause(cr); // Debugging
	/*AE*/
}

void Solver::detachClause(CRef cr, bool strict) {
	const Clause& c = ca[cr];
	if (c.size() < 2) {
		printClause(cr);
		std::clog << "clausesize: " << c.size() << "\n";
	}assert(c.size() > 1);

	if (strict) {
		remove(watches[~c[0]], Watcher(cr, c[1]));
		remove(watches[~c[1]], Watcher(cr, c[0]));
	} else {
		// Lazy detaching: (NOTE! Must clean all watcher lists before garbage collecting this clause)
		watches.smudge(~c[0]);
		watches.smudge(~c[1]);
	}

	if (c.learnt())
		learnts_literals -= c.size();
	else
		clauses_literals -= c.size();
}

/*AB*/
void Solver::saveState() {
	savedok = ok;
	savedlevel = decisionLevel();
	savedclausessize = clauses.size();
	remove_satisfied = false;
	savedqhead = qhead;
	trail_lim.copyTo(savedtraillim);
	trail.copyTo(savedtrail);
}

void Solver::resetState() {
	//FIXME is this correct and sufficient?
	ok = savedok;
	cancelUntil(savedlevel);
	qhead = savedqhead;
	trail.clear();
	savedtrail.copyTo(trail);
	trail_lim.clear();
	savedtraillim.copyTo(trail_lim);

	//Remove new clauses
	for (int i = savedclausessize; i < clauses.size(); i++) {
		removeClause(clauses[i]);
	}
	clauses.shrink(savedclausessize);

	//Remove learned clauses //TODO only forgetting the new learned clauses would also do and be better for learning!
	for (int i = 0; i < learnts.size(); i++) {
		removeClause(learnts[i]);
	}
	learnts.clear();
}
/*AE*/

void Solver::removeClause(CRef cr) {
	Clause& c = ca[cr];
	detachClause(cr);
	// Don't leave pointers to free'd memory!
	if (locked(c))
		vardata[var(c[0])].reason = CRef_Undef;
	c.mark(1);
	ca.free(cr);
}

bool Solver::satisfied(const Clause& c) const {
	for (int i = 0; i < c.size(); i++)
		if (value(c[i]) == l_True)
			return true;
	return false;
}

// Revert to the state at given level (keeping all assignment at 'level' but not beyond).
//
void Solver::cancelUntil(int level) {
	if (decisionLevel() > level) {
		/*A*/fullassignment = false;
		/*A*/
		Lit decision = trail[trail_lim[level]];
		for (int c = trail.size() - 1; c >= trail_lim[level]; c--) {
			Var x = var(trail[c]);
			assigns[x] = l_Undef;
			if (phase_saving > 1 || ((phase_saving == 1) && c > trail_lim.last()))
				polarity[x] = sign(trail[c]);
			insertVarOrder(x);
		}
		qhead = trail_lim[level];
		trail.shrink(trail.size() - trail_lim[level]);
		/*AB*/
		int levels = trail_lim.size() - level;
		trail_lim.shrink(levels);
		getPCSolver().backtrackDecisionLevel(level, decision);
		/*AE*/
	}
	/*if(level==0){
		cerr <<"Root certainties: ";
		for(int i=0; i<trail.size(); ++i){
			cerr<<(sign(trail[i])?"-":"") <<var(trail[i])+1 <<", ";
		}
		cerr <<"\n";
	}*/
}

//=================================================================================================
// Major methods:

Lit Solver::pickBranchLit() {
	Var next = var_Undef;

	// Random decision:
	if (drand(random_seed) < random_var_freq && !order_heap.empty()) {
		next = order_heap[irand(random_seed, order_heap.size())];
		if (value(next) == l_Undef && decision[next]) {
			rnd_decisions++;
		}
	}

	// Activity based decision:
	bool start = true;
	while (next == var_Undef || value(next) != l_Undef || !decision[next]) {
		if (!start) { // So then remove it if it proved redundant
			order_heap.removeMin();
		}
		start = false;

		if (order_heap.empty()) {
			next = var_Undef;
			break;
		} else {
			//next = order_heap.removeMin(); //REMOVES the next choice from the heap
			next = order_heap.peek(); //Does NOT remove this
		}
	}

	/*AB*/
	if (usecustomheur && next != var_Undef) {
		if (drand(random_seed) < customheurfreq) {
			if (customheurfreq > 0.25) {
				customheurfreq -= 0.01;
			}
			next = getPCSolver().changeBranchChoice(next);
		}
	} else {
		if (!start && next != var_Undef) {
			order_heap.removeMin();
		}
	}
	/*AE*/

	// Choose polarity based on different polarity modes (global or per-variable):
	if (next == var_Undef)
		return lit_Undef;
	else if (user_pol[next] != l_Undef)
		return mkLit(next, user_pol[next] == l_True);
	else if (rnd_pol)
		return mkLit(next, drand(random_seed) < 0.5);
	else
		return mkLit(next, polarity[next]);
}

/*_________________________________________________________________________________________________
 |
 |  analyze : (confl : Clause*) (out_learnt : vec<Lit>&) (out_btlevel : int&)  ->  [void]
 |
 |  Description:
 |    Analyze conflict and produce a reason clause.
 |
 |    Pre-conditions:
 |      * 'out_learnt' is assumed to be cleared.
 |      * Current decision level must be greater than root level.
 |
 |    Post-conditions:
 |      * 'out_learnt[0]' is the asserting literal at level 'out_btlevel'.
 |      * If out_learnt.size() > 1 then 'out_learnt[1]' has the greatest decision level of the
 |        rest of literals. There may be others from the same level though.
 |
 |________________________________________________________________________________________________@*/
bool Solver::isAlreadyUsedInAnalyze(const Lit& lit) const {
	return seen[var(lit)] == 1;
}

void Solver::analyze(CRef confl, vec<Lit>& out_learnt, int& out_btlevel) {
	int pathC = 0;
	Lit p = lit_Undef;

	/*AB VERY IMPORTANT*/
	int lvl = 0;
	Clause& c = ca[confl];
	for (int i = 0; i < c.size(); i++) {
		int litlevel = level(var(c[i]));
		if (litlevel > lvl) {
			lvl = litlevel;
		}
	}

	assert(lvl<=decisionLevel());

	cancelUntil(lvl);

	assert(confl!=CRef_Undef);
	assert(lvl==decisionLevel());

	//reportf("Conflicts: %d.\n", conflicts);
	std::vector<Lit> explain;
	/*AE*/

	// Generate conflict clause:
	//
	out_learnt.push(); // (leave room for the asserting literal)
	int index = trail.size() - 1;

	/*A*/
	bool deleteImplicitClause = false;
	do {
		assert(confl != CRef_Undef);
		// (otherwise should be UIP)
		Clause& c = ca[confl];

		/*AB*/
		if (verbosity > 4) {
			clog << "DECISION LEVEL " << decisionLevel() << "\n";
			clog << "Current conflict clause: ";
			printClause(confl);
			clog << "\n";
			clog << "Current learned clause: ";
			for (int i = 1; i < out_learnt.size(); i++) {
				clog << out_learnt[i] << " ";
			}
			clog << "\n";
			clog << "Still explain: ";
		}
		/*AE*/

		if (c.learnt())
			claBumpActivity(c);

		for (int j = (p == lit_Undef) ? 0 : 1; j < c.size(); j++) {
			Lit q = c[j];

			if (!seen[var(q)] && level(var(q)) > 0) {
				varBumpActivity(var(q));
				seen[var(q)] = 1;
				if (level(var(q)) >= decisionLevel())
					pathC++;
				else
					out_learnt.push(q);
			}
		}

		/*AB*/
		if (verbosity > 4) {
			for (std::vector<Lit>::const_iterator i = explain.begin(); i < explain.end(); i++) {
				clog << *i << " ";
			}
			clog << "\n";
		}

		if (deleteImplicitClause) {
			ca.free(confl);
			deleteImplicitClause = false;
		}
		/*AE*/

		// Select next clause to look at:
		while (!seen[var(trail[index--])])
			;
		p = trail[index + 1];
		confl = reason(var(p));

		/*AB*/
		if (verbosity > 4) {
			clog << "Getting explanation for ";
			for (std::vector<Lit>::iterator i = explain.begin(); i < explain.end(); i++) {
				if (var(*i) == var(p)) {
					explain.erase(i);
					break;
				}
			}
			clog << p << "\n";
		}

		if (confl == CRef_Undef && pathC > 1) {
			confl = getPCSolver().getExplanation(p);
			deleteImplicitClause = true;
		}
		if (verbosity > 4 && confl != CRef_Undef) {
			reportf("Explanation is ");
			printClause(confl);
			reportf("\n");
		}
		/*AE*/

		seen[var(p)] = 0;
		pathC--;

	} while (pathC > 0);
	out_learnt[0] = ~p;

	// Simplify conflict clause:
	//
	int i, j;
	out_learnt.copyTo(analyze_toclear);
	if (ccmin_mode == 2) {
		uint32_t abstract_level = 0;
		for (i = 1; i < out_learnt.size(); i++)
			abstract_level |= abstractLevel(var(out_learnt[i])); // (maintain an abstraction of levels involved in conflict)

		for (i = j = 1; i < out_learnt.size(); i++)
			if (reason(var(out_learnt[i])) == CRef_Undef || !litRedundant(out_learnt[i], abstract_level))
				out_learnt[j++] = out_learnt[i];

	} else if (ccmin_mode == 1) {
		for (i = j = 1; i < out_learnt.size(); i++) {
			Var x = var(out_learnt[i]);

			if (reason(x) == CRef_Undef)
				out_learnt[j++] = out_learnt[i];
			else {
				Clause& c = ca[reason(var(out_learnt[i]))];
				for (int k = 1; k < c.size(); k++)
					if (!seen[var(c[k])] && level(var(c[k])) > 0) {
						out_learnt[j++] = out_learnt[i];
						break;
					}
			}
		}
	} else
		i = j = out_learnt.size();

	max_literals += out_learnt.size();
	out_learnt.shrink(i - j);
	tot_literals += out_learnt.size();

	// Find correct backtrack level:
	//
	if (out_learnt.size() == 1)
		out_btlevel = 0;
	else {
		int max_i = 1;
		// Find the first literal assigned at the next-highest level:
		for (int i = 2; i < out_learnt.size(); i++)
			if (level(var(out_learnt[i])) > level(var(out_learnt[max_i])))
				max_i = i;
		// Swap-in this literal at index 1:
		Lit p = out_learnt[max_i];
		out_learnt[max_i] = out_learnt[1];
		out_learnt[1] = p;
		out_btlevel = level(var(p));
	}

	for (int j = 0; j < analyze_toclear.size(); j++)
		seen[var(analyze_toclear[j])] = 0; // ('seen[]' is now cleared)

	/*A*/
}

// Check if 'p' can be removed. 'abstract_levels' is used to abort early if the algorithm is
// visiting literals at levels that cannot be removed later.
bool Solver::litRedundant(Lit p, uint32_t abstract_levels) {
	analyze_stack.clear();
	analyze_stack.push(p);
	int top = analyze_toclear.size();
	while (analyze_stack.size() > 0) {
		assert(reason(var(analyze_stack.last())) != CRef_Undef);
		Clause& c = ca[reason(var(analyze_stack.last()))];
		analyze_stack.pop();

		for (int i = 1; i < c.size(); i++) {
			Lit p = c[i];
			if (!seen[var(p)] && level(var(p)) > 0) {
				if (reason(var(p)) != CRef_Undef && (abstractLevel(var(p)) & abstract_levels) != 0) {
					seen[var(p)] = 1;
					analyze_stack.push(p);
					analyze_toclear.push(p);
				} else {
					for (int j = top; j < analyze_toclear.size(); j++)
						seen[var(analyze_toclear[j])] = 0;
					analyze_toclear.shrink(analyze_toclear.size() - top);
					return false;
				}
			}
		}
	}

	return true;
}

/*_________________________________________________________________________________________________
 |
 |  analyzeFinal : (p : Lit)  ->  [void]
 |
 |  Description:
 |    Specialized analysis procedure to express the final conflict in terms of assumptions.
 |    Calculates the (possibly empty) set of assumptions that led to the assignment of 'p', and
 |    stores the result in 'out_conflict'.
 |________________________________________________________________________________________________@*/
void Solver::analyzeFinal(Lit p, vec<Lit>& out_conflict) {
	out_conflict.clear();
	out_conflict.push(p);

	if (decisionLevel() == 0)
		return;

	seen[var(p)] = 1;

	for (int i = trail.size() - 1; i >= trail_lim[0]; i--) {
		Var x = var(trail[i]);
		if (seen[x]) {
			if (reason(x) == CRef_Undef) {
				assert(level(x) > 0);
				out_conflict.push(~trail[i]);
			} else {
				Clause& c = ca[reason(x)];
				for (int j = 1; j < c.size(); j++)
					if (level(var(c[j])) > 0)
						seen[var(c[j])] = 1;
			}
			seen[x] = 0;
		}
	}

	seen[var(p)] = 0;
}

void Solver::uncheckedEnqueue(Lit p, CRef from) {
	assert(value(p) == l_Undef);
	assigns[var(p)] = lbool(!sign(p));
	vardata[var(p)] = mkVarData(from, decisionLevel());
	trail.push_(p);
	/*A*/
	if(not isDecisionVar(var(p))){
		setDecidable(var(p), true);
	}
	getPCSolver().notifySetTrue(p);
	/*A*/if (verbosity > 3) {
		getPCSolver().printEnqueued(p);
	}
}

/*_________________________________________________________________________________________________
 |
 |  propagate : [void]  ->  [Clause*]
 |
 |  Description:
 |    Propagates all enqueued facts. If a conflict arises, the conflicting clause is returned,
 |    otherwise CRef_Undef.
 |
 |    Post-conditions:
 |      * the propagation queue is empty, even if there was a conflict.
 |________________________________________________________________________________________________@*/
CRef Solver::propagate() {
	return getPCSolver().propagate();
}

CRef Solver::notifypropagate() {
	CRef confl = CRef_Undef;
	int num_props = 0;
	watches.cleanAll();

	while (qhead < trail.size()) {
		Lit p = trail[qhead++]; // 'p' is enqueued fact to propagate.
		vec<Watcher>& ws = watches[p];
		Watcher *i, *j, *end;
		num_props++;

		for (i = j = (Watcher*) ws, end = i + ws.size(); i != end;) {
			// Try to avoid inspecting the clause:
			// FIXME do not understand blocker code yet, so commented it
			Lit blocker = i->blocker;
			if (value(blocker) == l_True) {
				setDecidable(var(blocker), true); // TODO is this the best possible call?
				*j++ = *i++;
				continue;
			}

			// Make sure the false literal is data[1]:
			CRef cr = i->cref;
			Clause& c = ca[cr];
			assert(isDecisionVar(var(c[0])) || isDecisionVar(var(c[1])));
			Lit false_lit = ~p;
			if (c[0] == false_lit){
				c[0] = c[1], c[1] = false_lit;
			}
			assert(c[1] == false_lit);
			i++;

			// If 0th watch is true, then clause is already satisfied.
			Lit first = c[0];
			Watcher w = Watcher(cr, first);
			if (first != blocker && value(first) == l_True) { // TODO blocker
				*j++ = w;
				/*AB*/
				checkDecisionVars(c);
				/*AE*/
				continue;
			}

			// Look for new watch:
			for (int k = 2; k < c.size(); k++){
				if (value(c[k]) != l_False) {
					c[1] = c[k];
					c[k] = false_lit;
					watches[~c[1]].push(w);
					/*AB*/
					checkDecisionVars(c);
					/*AE*/
					goto NextClause;
				}
			}

			// Did not find watch -- clause is unit under assignment:
			*j++ = w;
			if (value(first) == l_False) { // NOTE: conflict during unit propagation
				confl = cr;
				qhead = trail.size();
				// Copy the remaining watches: (NOTE: will cause loop to be false)
				while (i < end){
					*j++ = *i++;
				}
			} else{
				uncheckedEnqueue(first, cr);
				/*AB*/
				checkDecisionVars(c);
				/*AE*/
			}

			NextClause: ;
		}
		ws.shrink(i - j);
		/*AB*/
		if (confl != CRef_Undef) {
			qhead = trail.size();
		}
		/*AE*/
	}
	propagations += num_props;
	simpDB_props -= num_props;

	return confl;
}

/*_________________________________________________________________________________________________
 |
 |  reduceDB : ()  ->  [void]
 |
 |  Description:
 |    Remove half of the learnt clauses, minus the clauses locked by the current assignment. Locked
 |    clauses are clauses that are reason to some assignment. Binary clauses are never removed.
 |________________________________________________________________________________________________@*/
struct reduceDB_lt {
	ClauseAllocator& ca;
	reduceDB_lt(ClauseAllocator& ca_)
			: ca(ca_) {
	}
	bool operator ()(CRef x, CRef y) {
		return ca[x].size() > 2 && (ca[y].size() == 2 || ca[x].activity() < ca[y].activity());
	}
};
void Solver::reduceDB() {
	int i, j;
	double extra_lim = cla_inc / learnts.size(); // Remove any clause below this activity

	sort(learnts, reduceDB_lt(ca));
	// Don't delete binary or locked clauses. From the rest, delete clauses from the first half
	// and clauses with activity smaller than 'extra_lim':
	for (i = j = 0; i < learnts.size(); i++) {
		Clause& c = ca[learnts[i]];
		if (c.size() > 2 && !locked(c) && (i < learnts.size() / 2 || c.activity() < extra_lim))
			removeClause(learnts[i]);
		else
			learnts[j++] = learnts[i];
	}
	learnts.shrink(i - j);
	checkGarbage();
}

void Solver::removeSatisfied(vec<CRef>& cs) {
	int i, j;
	for (i = j = 0; i < cs.size(); i++) {
		Clause& c = ca[cs[i]];
		if (satisfied(c))
			removeClause(cs[i]);
		else
			cs[j++] = cs[i];
	}
	cs.shrink(i - j);
}

void Solver::rebuildOrderHeap() {
	vec<Var> vs;
	for (Var v = 0; v < nVars(); v++)
		if (decision[v] && value(v) == l_Undef)
			vs.push(v);
	order_heap.build(vs);
}

/*_________________________________________________________________________________________________
 |
 |  simplify : [void]  ->  [bool]
 |
 |  Description:
 |    Simplify the clause database according to the current top-level assigment. Currently, the only
 |    thing done here is the removal of satisfied clauses, but more things can be put here.
 |________________________________________________________________________________________________@*/
bool Solver::simplify() {
	assert(decisionLevel() == 0);

	if (!ok || propagate() != CRef_Undef)
		return ok = false;

	if (nAssigns() == simpDB_assigns || (simpDB_props > 0))
		return true;

	// Remove satisfied clauses:
	removeSatisfied(learnts);
	if (remove_satisfied) // Can be turned off.
		removeSatisfied(clauses);
	checkGarbage();
	rebuildOrderHeap();

	simpDB_assigns = nAssigns();
	simpDB_props = clauses_literals + learnts_literals; // (shouldn't depend on stats really, but it will do for now)

	return true;
}

/*_________________________________________________________________________________________________
 |
 |  search : (nof_conflicts : int) (params : const SearchParams&)  ->  [lbool]
 |
 |  Description:
 |    Search for a model the specified number of conflicts.
 |    NOTE! Use negative value for 'nof_conflicts' indicate infinity.
 |
 |  Output:
 |    'l_True' if a partial assigment that is consistent with respect to the clauseset is found. If
 |    all variables are decision variables, this means that the clause set is satisfiable. 'l_False'
 |    if the clause set is unsatisfiable. 'l_Undef' if the bound on number of conflicts is reached.
 |________________________________________________________________________________________________@*/
lbool Solver::search(int nof_conflicts/*AB*/, bool nosearch/*AE*/) {
	assert(ok);
	int backtrack_level;
	int conflictC = 0;
	vec<Lit> learnt_clause;
	starts++;

	CRef confl = CRef_Undef;
	bool fullassignmentconflict = false;

	for (;;) {
		if (terminateRequested()) {
			return l_Undef;
		}
		if (!ok) {
			return l_False;
		}
		if (!fullassignmentconflict) {
			confl = propagate();
		}
		fullassignmentconflict = false;

		if (!ok) {
			return l_False;
		}

		if (confl != CRef_Undef) {
			// CONFLICT
			conflicts++;
			conflictC++;
			if (decisionLevel() == 0){
				return l_False;
			}

			learnt_clause.clear();

			analyze(confl, learnt_clause, backtrack_level);

			cancelUntil(backtrack_level);

			//FIXME inconsistency with addLearnedClause method
			if (learnt_clause.size() == 1) {
				uncheckedEnqueue(learnt_clause[0]);
			} else {
				CRef cr = ca.alloc(learnt_clause, true);
				addToClauses(cr, true);
				attachClause(cr);
				claBumpActivity(ca[cr]);
				uncheckedEnqueue(learnt_clause[0], cr);
			}

			varDecayActivity();
			claDecayActivity();

			if (--learntsize_adjust_cnt == 0) {
				learntsize_adjust_confl *= learntsize_adjust_inc;
				learntsize_adjust_cnt = (int) learntsize_adjust_confl;
				max_learnts *= learntsize_inc;

				if (verbosity >= 1)
					printf("| %9d | %7d %8d %8d | %8d %8d %6.0f | %6.3f %% |\n", (int) conflicts,
							(int) dec_vars - (trail_lim.size() == 0 ? trail.size() : trail_lim[0]), nClauses(), (int) clauses_literals, (int) max_learnts,
							nLearnts(), (double) learnts_literals / nLearnts(), progressEstimate() * 100);
			}

		} else {
			// NO CONFLICT
			if ((nof_conflicts >= 0 && conflictC >= nof_conflicts) || !withinBudget()) {
				// Reached bound on number of conflicts:
				progress_estimate = progressEstimate();
				cancelUntil(0);
				return l_Undef;
			}

			// Simplify the set of problem clauses:
			if (decisionLevel() == 0 && !simplify()){
				return l_False;
			}

			if (learnts.size() - nAssigns() >= max_learnts){
				// Reduce the set of learnt clauses:
				reduceDB();
			}

			Lit next = lit_Undef;
			while (decisionLevel() < assumptions.size()) {
				// Perform user provided assumption:
				Lit p = assumptions[decisionLevel()];
				if (value(p) == l_True) {
					// Dummy decision level:
					createNewDecisionLevel();
				} else if (value(p) == l_False) {
					analyzeFinal(~p, conflict);
					return l_False;
				} else {
					next = p;
					break;
				}
			}

			if (next == lit_Undef) {
				/*AB*/
				if (nosearch) {
					return l_True;
				}
				/*AE*/

				// New variable decision:
				decisions++;
				next = pickBranchLit();

				if (next == lit_Undef) {
					fullassignment = true;

					confl = getPCSolver().checkFullAssignment(); // NOTE: can backtrack as any propagator, so in that case should not stop
					if (order_heap.size()>0 || qhead!=trail.size()) {
						continue;
					}

					if (confl == CRef_Undef) { // Assignment is a model
						//cerr << "Model: ";
						//for (auto i = 0; i < nbVars(); i++) {
						//	auto lit = mkLit(i);
						//	cerr << (sign(lit) ? "-" : "") << var(lit) + 1 << ":" << (value(lit) == l_True ? '1' : (value(lit) == l_False ? '0' : 'X')) << " ";
						//}
						return l_True;
					} else {
						fullassignmentconflict = true;
					}
				}

				/*AB*/
				if (verbosity > 3) {
					getPCSolver().printChoiceMade(decisionLevel(), next);
				}
				/*AE*/
			}

			// Increase decision level and enqueue 'next'
			if (!fullassignmentconflict) {
				createNewDecisionLevel();
				uncheckedEnqueue(next);
			}
		}
	}
}

double Solver::progressEstimate() const {
	double progress = 0;
	double F = 1.0 / nVars();

	for (int i = 0; i <= decisionLevel(); i++) {
		int beg = i == 0 ? 0 : trail_lim[i - 1];
		int end = i == decisionLevel() ? trail.size() : trail_lim[i];
		progress += pow(F, i) * (end - beg);
	}

	return progress / nVars();
}

/*
 Finite subsequences of the Luby-sequence:

 0: 1
 1: 1 1 2
 2: 1 1 2 1 1 2 4
 3: 1 1 2 1 1 2 4 1 1 2 1 1 2 4 8
 ...


 */

static double luby(double y, int x) {
	// Find the finite subsequence that contains index 'x', and the
	// size of that subsequence:
	int size, seq;
	for (size = 1, seq = 0; size < x + 1; seq++, size = 2 * size + 1)
		;

	while (size - 1 != x) {
		size = (size - 1) >> 1;
		seq--;
		x = x % size;
	}

	return pow(y, seq);
}

// NOTE: assumptions passed in member-variable 'assumptions'.
lbool Solver::solve_(/*AB*/bool nosearch/*AE*/) {
	model.clear();
	conflict.clear();
	if (!ok)
		return l_False;

	solves++;

	// To get a better estimate of the number of max_learnts allowed, have to ask all propagators their size
	max_learnts = getPCSolver().getNbOfFormulas() * learntsize_factor;
	learntsize_adjust_confl = learntsize_adjust_start_confl;
	learntsize_adjust_cnt = (int) learntsize_adjust_confl;
	lbool status = l_Undef;

	/*AB*/
	/*if (verbosity >= 1){
	 printf("============================[ Search Statistics ]==============================\n");
	 printf("| Conflicts |          ORIGINAL         |          LEARNT          | Progress |\n");
	 printf("|           |    Vars  Clauses Literals |    Limit  Clauses Lit/Cl |          |\n");
	 printf("===============================================================================\n");
	 }*/
	/*AE*/

	// Search:
	int curr_restarts = 0;
	while (status == l_Undef) {
		if (terminateRequested()) {
			return l_Undef;
		}
		double rest_base = luby_restart ? luby(restart_inc, curr_restarts) : pow(restart_inc, curr_restarts);
		status = search(rest_base * restart_first/*AB*/, nosearch/*AE*/);
		if (terminateRequested()) {
			return l_Undef;
		}
		/*AB*/
		if (nosearch) {
			return status;
		}
		/*AE*/
		if (!withinBudget())
			break;
		curr_restarts++;
	}

	/*AB*/
	/*if (verbosity >= 1)
	 printf("===============================================================================\n");
	 */
	/*AE*/

	if (status == l_True) {
		// Extend & copy model:
		model.growTo(nVars());
		for (int i = 0; i < nVars(); i++){
			model[i] = value(i);
		}
#ifndef NDEBUG
		for(int i=0; i<nbClauses(); ++i){
			auto c = getClause(i);
			bool clausetrue = false, clauseHasNonFalseDecidable = false;
			for(int j=0; j<getClauseSize(c); ++j){
				if(not isFalse(getClauseLit(c, j)) && isDecisionVar(var(getClauseLit(c, j)))){
					clauseHasNonFalseDecidable = true;
				}
				if(isTrue(getClauseLit(c, j))){
					clausetrue = true;
				}
			}
			if(not clausetrue || not clauseHasNonFalseDecidable){
				clog <<(clausetrue?"True":"False") <<", " <<(clauseHasNonFalseDecidable?"decidable":"undecidable") <<" clause ";
				printClause(c);
			}
			assert(clausetrue && clauseHasNonFalseDecidable);
		}
#endif
	} else if (status == l_False && conflict.size() == 0)
		ok = false;

	/*A*/ //cancelUntil(0);
	return status;
}

//=================================================================================================
// Writing CNF to DIMACS:
// 
static Var mapVar(Var x, vec<Var>& map, Var& max) {
	if (map.size() <= x || map[x] == -1) {
		map.growTo(x + 1, -1);
		map[x] = max++;
	}
	return map[x];
}

void Solver::toDimacs(FILE* f, Clause& c, vec<Var>& map, Var& max) {
	if (satisfied(c))
		return;

	for (int i = 0; i < c.size(); i++)
		if (value(c[i]) != l_False)
			fprintf(f, "%s%d ", sign(c[i]) ? "-" : "", mapVar(var(c[i]), map, max) + 1);
	fprintf(f, "0\n");
}

void Solver::toDimacs(const char *file, const vec<Lit>& assumps) {
	FILE* f = fopen(file, "wr");
	if (f == NULL)
		fprintf(stderr, "could not open file %s\n", file), exit(1);
	toDimacs(f, assumps);
	fclose(f);
}

void Solver::toDimacs(FILE* f, const vec<Lit>& assumps) {
	// Handle case when solver is in contradictory state:
	if (!ok) {
		fprintf(f, "p cnf 1 2\n1 0\n-1 0\n");
		return;
	}

	vec<Var> map;
	Var max = 0;

	// Cannot use removeClauses here because it is not safe
	// to deallocate them at this point. Could be improved.
	int cnt = 0;
	for (int i = 0; i < clauses.size(); i++)
		if (!satisfied(ca[clauses[i]]))
			cnt++;

	for (int i = 0; i < clauses.size(); i++)
		if (!satisfied(ca[clauses[i]])) {
			Clause& c = ca[clauses[i]];
			for (int j = 0; j < c.size(); j++)
				if (value(c[j]) != l_False)
					mapVar(var(c[j]), map, max);
		}

	// Assumptions are added as unit clauses:
	cnt += assumps.size();

	fprintf(f, "p cnf %d %d\n", max, cnt);

	for (int i = 0; i < assumps.size(); i++) {
		assert(value(assumps[i]) != l_False);
		fprintf(f, "%s%d 0\n", sign(assumps[i]) ? "-" : "", mapVar(var(assumps[i]), map, max) + 1);
	}

	for (int i = 0; i < clauses.size(); i++)
		toDimacs(f, ca[clauses[i]], map, max);

	if (verbosity > 0)
		printf("Wrote %d clauses with %d variables.\n", cnt, max);
}

/*AB*/
void Solver::printClause(CRef rc) const {
	const Clause& c = ca[rc];
	bool begin = true;
	for (int i = 0; i < c.size(); i++){
		if(not begin){
			clog <<" & ";
		}
		begin = false;
		clog <<c[i] <<"(" <<(value(c[i]) == l_True ? '1' : (value(c[i]) == l_False ? '0' : 'X')) <<")";
	}
	clog <<"\n";
}
/*AE*/

//=================================================================================================
// Garbage Collection methods:
void Solver::relocAll(ClauseAllocator& to) {
	// All watchers:
	//
	// for (int i = 0; i < watches.size(); i++)
	watches.cleanAll();
	for (int v = 0; v < nVars(); v++)
		for (int s = 0; s < 2; s++) {
			Lit p = mkLit(v, s);
			// printf(" >>> RELOCING: %s%d\n", sign(p)?"-":"", var(p)+1);
			vec<Watcher>& ws = watches[p];
			for (int j = 0; j < ws.size(); j++)
				ca.reloc(ws[j].cref, to);
		}

	// All reasons:
	//
	for (int i = 0; i < trail.size(); i++) {
		Var v = var(trail[i]);

		if (reason(v) != CRef_Undef && (ca[reason(v)].reloced() || locked(ca[reason(v)])))
			ca.reloc(vardata[v].reason, to);
	}

	// All learnt:
	//
	for (int i = 0; i < learnts.size(); i++)
		ca.reloc(learnts[i], to);

	// All original:
	//
	for (int i = 0; i < clauses.size(); i++)
		ca.reloc(clauses[i], to);
}

void Solver::garbageCollect() {
	// Initialize the next region to a size corresponding to the estimated utilization degree. This
	// is not precise but should avoid some unnecessary reallocations for the new region:
	ClauseAllocator to(ca.size() - ca.wasted());

	relocAll(to);
	if (verbosity >= 2)
		printf("|  Garbage collection:   %12d bytes => %12d bytes             |\n", ca.size() * ClauseAllocator::Unit_Size,
				to.size() * ClauseAllocator::Unit_Size);
	to.moveTo(ca);
}

/*AB*/
void Solver::printStatistics() const {
	std::clog << "> restarts              : " << starts << "\n";
	std::clog << "> conflicts             : " << decisions << "  (" << (float) rnd_decisions * 100 / (float) decisions << " % random)\n";
	std::clog << "> decisions             : " << starts << "\n";
	std::clog << "> propagations          : " << propagations << "\n";
	std::clog << "> conflict literals     : " << tot_literals << "  (" << ((max_literals - tot_literals) * 100 / (double) max_literals) << " % deleted)\n";
}

int Solver::printECNF(std::ostream& stream, std::set<Var>& printedvars) {
	if (not okay()) {
		stream << "0\n";
		return 0;
	}
	for (int i = 0; i < clauses.size(); ++i) {
		const Clause& clause = ca[clauses[i]];
		stringstream ss;
		bool clausetrue = false;
		for (int j = 0; not clausetrue && j < clause.size(); ++j) {
			Lit lit = clause[j];
			lbool val = value(lit);
			if (val == l_Undef) {
				ss << (sign(lit) ? -(var(lit) + 1) : var(lit) + 1) << " ";
				printedvars.insert(var(lit));
			} else if (val == l_True) {
				clausetrue = true;
			}
		}
		if (not clausetrue) {
			ss << "0\n";
			stream << ss.str();
		}
	}

	// Print implied literals
	int lastrootassertion = trail.size();
	if (trail_lim.size() > 0) {
		lastrootassertion = trail_lim[0];
	}
	for (int i = 0; i < lastrootassertion; ++i) {
		Lit lit = trail[i];
		// TODO should only print literals which have a translation?
		stream << (sign(lit) ? -(var(lit) + 1) : var(lit) + 1) << " 0\n";
	}

	return clauses.size() + lastrootassertion;
}

/*AE*/
