#include "internal.hpp"

namespace CaDiCaL {

#define FACTORS 1
#define QUOTIENT 2
#define NOUNTED 4

inline bool factor_occs_size::operator() (unsigned a, unsigned b) {
  size_t s = internal->occs (internal->u2i (a)).size ();
  size_t t = internal->occs (internal->u2i (b)).size ();
  if (s > t)
    return true;
  if (s < t)
    return false;
  return a > b;
}

// do full occurence list as in elim.cpp but filter out useless clauses
void Internal::factor_mode () {
  reset_watches ();

  assert (!watching ());
  init_occs ();

  const int size_limit = opts.factorsize;

  vector<unsigned> bincount, largecount;
  const unsigned max_lit = 2 * (max_var + 1);
  enlarge_zero (bincount, max_lit);
  if (size_limit > 2)
    enlarge_zero (largecount, max_lit);

  vector<Clause *> candidates;

  // push binary clauses on the occurrence stack.
  for (const auto &c : clauses) {
    if (c->garbage) continue;
    if (c->redundant && c->size > 2) continue;
    if (c->size > size_limit) continue;
    if (c->size == 2) {
      const int lit = c->literals[0];
      const int other = c->literals[1];
      bincount[vlit (lit)]++;
      bincount[vlit (other)]++;
      occs (lit).push_back (c);
      occs (other).push_back (c);
      continue;
    }
    candidates.push_back (c);
    for (const auto &lit : *c) {
      largecount[vlit (lit)]++;
    }
  }
  if (size_limit == 2) return;

  // iterate counts of larger clauses rounds often
  const unsigned rounds = opts.factorcandrounds;
  for (unsigned round = 1; round <= rounds; round++) {
    vector<unsigned> newlargecount;
    enlarge_zero (newlargecount, max_lit);
    const auto begin = candidates.begin ();
    auto p = candidates.begin ();
    auto q = p;
    const auto end = candidates.end ();
    while (p != end) {
      Clause *c = *q++ = *p++;
      for (const auto &lit : *c) {
        const auto idx = vlit (lit);
        if (bincount[idx] + largecount[idx] < 2) {
          q--;
          goto CONTINUE_WITH_NEXT_CLAUSE;
        }
      }
      for (const auto &lit : *c) {
        const auto idx = vlit (lit);
        newlargecount[idx]++;
      }
    CONTINUE_WITH_NEXT_CLAUSE:
      continue;
    }
    candidates.resize (q - begin);
    largecount.swap (newlargecount);
  }

  // finally push remaining clause on the occurrence stack
  for (const auto &c : candidates)
    for (const auto &lit : *c)
      occs (lit).push_back (c);

}

// go back to two watch scheme
void Internal::reset_factor_mode () {
  reset_occs ();
  init_watches ();
  connect_watches ();
}

Factoring::Factoring (Internal *i, int64_t l)
    : internal (i), limit (l), schedule (i) {
  const unsigned max_var = internal->max_var;
  const unsigned max_lit = 2 * (max_var + 1);
  initial = allocated = size = max_var;
  bound = internal->lim.elimbound;
  enlarge_zero (count, max_lit);
  quotients.first = quotients.last = 0;
}

Factoring::~Factoring () {
  assert (counted.empty ());
  assert (nounted.empty ());
  assert (flauses.empty ());
  internal->release_quotients (*this);
  schedule.erase ();  // actually not necessary
}

double Internal::tied_next_factor_score (int lit) {
  double res = occs (lit).size ();
  LOG ("watches score %g of %d", res, lit);
  return res;
}

// the marks in cadical have 6 bits for marking but work on idx
// to mark everything (FACTORS, QUOTIENT, NOUNTED) we shift the bits
// depending on the sign of factor (+ bitmask)
// i.e. if factor is positive, we apply a bitmask to only get
// the first three bits (& 7u)
// otherwise we leftshift by 3 (>> 3) to get the bits 4,5,6
// use markfact, unmarkfact, getfact for this purpose.
//
Quotient *Internal::new_quotient (Factoring &factoring, int factor) {
  assert (!getfact (factor, FACTORS));
  markfact (factor, FACTORS);
  Quotient *res = new Quotient (factor);
  res->next = 0;
  res->matched = 0;
  Quotient *last = factoring.quotients.last;
  res->bid = 0;
  if (last) {
    assert (factoring.quotients.first);
    assert (!last->next);
    last->next = res;
    res->id = last->id + 1;
  } else {
    assert (!factoring.quotients.first);
    factoring.quotients.first = res;
    res->id = 0;
  }
  factoring.quotients.last = res;
  res->prev = last;
  LOG ("new quotient[%zu] with factor %d", res->id, factor);
  return res;
}


void Internal::release_quotients (Factoring &factoring) {
  for (Quotient *q = factoring.quotients.first, *next; q; q = next) {
    next = q->next;
    int factor = q->factor;
    assert (getfact (factor, FACTORS));
    unmarkfact (factor, FACTORS);
    delete q;
  }
  factoring.quotients.first = factoring.quotients.last = 0;
}

size_t Internal::first_factor (Factoring &factoring, int factor) {
  assert (!factoring.quotients.first);
  Quotient *quotient = new_quotient (factoring, factor);
  vector<Clause *> &qlauses = quotient->qlauses;
  int64_t ticks = 0;
  for (const auto &c : occs (factor)) {
    qlauses.push_back (c);
    ticks++;
  }
  size_t res = qlauses.size ();
  LOG ("quotient[0] factor %d size %zu", factor, res);
  // This invariant can of course be broken by previous factorings
  // assert (res > 1);
  stats.factor_ticks += ticks;
  return res;
}

void Internal::clear_nounted (vector<int> &nounted) {
  for (const auto &lit : nounted) {
    assert (getfact (lit, NOUNTED));
    unmarkfact (lit, NOUNTED);
  }
  nounted.clear ();
}

void Internal::clear_flauses (vector<Clause *> &flauses) {
  for (auto c : flauses) {
    assert (c->swept);
    c->swept = false;
  }
  flauses.clear ();
}

Quotient *Internal::best_quotient (Factoring &factoring,
                                size_t *best_reduction_ptr) {
  size_t factors = 1, best_reduction = 0;
  Quotient *best = 0;
  for (Quotient *q = factoring.quotients.first; q; q = q->next) {
    size_t quotients = q->qlauses.size ();
    size_t before_factorization = quotients * factors;
    size_t after_factorization = quotients + factors;
    if (before_factorization == after_factorization)
      LOG ("quotient[%zu] factors %zu clauses into %zu thus no change",
           factors - 1, before_factorization, after_factorization);
    else if (before_factorization < after_factorization)
      LOG ("quotient[%zu] factors %zu clauses into %zu thus %zu more",
           factors - 1, before_factorization, after_factorization,
           after_factorization - before_factorization);
    else {
      size_t delta = before_factorization - after_factorization;
      LOG ("quotient[%zu] factors %zu clauses into %zu thus %zu less",
           factors - 1, before_factorization, after_factorization, delta);
      if (!best || best_reduction < delta) {
        best_reduction = delta;
        best = q;
      }
    }
    factors++;
  }
  if (!best) {
    LOG ("no decreasing quotient found");
    return 0;
  }
  LOG ("best decreasing quotient[%zu] with reduction %zu", best->id,
       best_reduction);
  *best_reduction_ptr = best_reduction;
  return best;
}

int Internal::next_factor (Factoring &factoring,
                           unsigned *next_count_ptr) {
  Quotient *last_quotient = factoring.quotients.last;
  assert (last_quotient);
  vector<Clause *> &last_clauses = last_quotient->qlauses;
  vector<unsigned> &count = factoring.count;
  vector<int> &counted = factoring.counted;
  vector<Clause *> &flauses = factoring.flauses;
  assert (counted.empty ());
  assert (flauses.empty ());
  const int initial = factoring.initial;
  int64_t ticks = 1; // TODO: + kissat_cache_lines (SIZE_STACK (*last_clauses), sizeof (watch));
  for (auto c : last_clauses) {
    assert (!c->swept);
    int min_lit = 0;
    unsigned factors = 0;
    size_t min_size = 0;
    ticks++;
    for (const auto &other : *c) {
      if (getfact (other, FACTORS)) {
        if (factors++)
          break;
      } else {
        assert (!getfact (other, QUOTIENT));
        markfact (other, QUOTIENT);
        const size_t other_size = occs (other).size ();
        if (!min_lit || other_size < min_size) {
          min_lit = other;
          min_size = other_size;
        }
      }
    }
    assert (factors);
    if (factors == 1) {
      assert (min_lit);
      const int c_size = c->size;
      vector<int> &nounted = factoring.nounted;
      assert (nounted.empty ());
      ticks += 1; // + kissat_cache_lines (SIZE_WATCHES (*min_watches), sizeof (watch));
      for (auto d : occs (min_lit)) {
        if (c == d) continue;
        ticks++;
        if (d->swept) continue;
        if (d->size != c_size) continue;
        int next = 0;
        for (const auto &other : *d) {
          if (getfact (other, QUOTIENT))
            continue;
          if (getfact (other, FACTORS))
            goto CONTINUE_WITH_NEXT_MIN_WATCH;
          if (getfact (other, NOUNTED))
            goto CONTINUE_WITH_NEXT_MIN_WATCH;
          if (next)
            goto CONTINUE_WITH_NEXT_MIN_WATCH;
          next = other;
        }
        assert (next);
        if (abs (next) > abs (initial))
          continue;
        if (!active (next))
          continue;
        assert (!getfact (next, FACTORS));
        assert (!getfact (next, NOUNTED));
        markfact (next, NOUNTED);
        nounted.push_back (next);
        d->swept = true;
        flauses.push_back (d);
        if (!count[vlit (next)])
          counted.push_back (next);
        count[vlit (next)]++;
      CONTINUE_WITH_NEXT_MIN_WATCH:;
      }
      clear_nounted (nounted);
    }
    for (const auto &other : *c)
      if (getfact (other, QUOTIENT))
        unmarkfact (other, QUOTIENT);
    stats.factor_ticks += ticks;
    ticks = 0;
    if (stats.factor_ticks > factoring.limit)
      break;
  }
  clear_flauses (flauses);
  unsigned next_count = 0;
  int next = 0;
  if (stats.factor_ticks <= factoring.limit) {
    unsigned ties = 0;
    for (const auto &lit : counted) {
      const unsigned lit_count = count[vlit (lit)];
      if (lit_count < next_count)
        continue;
      if (lit_count == next_count) {
        assert (lit_count);
        ties++;
      } else {
        assert (lit_count > next_count);
        next_count = lit_count;
        next = lit;
        ties = 1;
      }
    }
    if (next_count < 2) {
      LOG ("next factor count %u smaller than 2", next_count);
      next = 0;
    } else if (ties > 1) {
      LOG ("found %u tied next factor candidate literals with count %u",
           ties, next_count);
      double next_score = -1;
      for (const auto &lit : counted) {
        const unsigned lit_count = count[vlit (lit)];
        if (lit_count != next_count)
          continue;
        double lit_score = tied_next_factor_score (lit);
        assert (lit_score >= 0);
        LOG ("score %g of next factor candidate %d", lit_score, lit);
        if (lit_score <= next_score)
          continue;
        next_score = lit_score;
        next = lit;
      }
      assert (next_score >= 0);
      assert (next);
      LOG ("best score %g of next factor %d", next_score, next);
    } else {
      assert (ties == 1);
      LOG ("single next factor %d with count %u", next,
           next_count);
    }
  }
  for (const auto &lit : counted)
    count[vlit (lit)] = 0;
  counted.clear ();
  assert (!next || next_count > 1);
  *next_count_ptr = next_count;
  return next;
}



void Internal::factorize_next (Factoring &factoring, int next,
                              unsigned expected_next_count) {
  Quotient *last_quotient = factoring.quotients.last;
  Quotient *next_quotient = new_quotient (factoring, next);

  assert (last_quotient);
  vector<Clause *> &last_clauses = last_quotient->qlauses;
  vector<Clause *> &next_clauses = next_quotient->qlauses;
  vector<size_t> &matches = next_quotient->matches;
  vector<Clause *> &flauses = factoring.flauses;
  assert (flauses.empty ());

  int64_t ticks = 1;
  //    1 + kissat_cache_lines (SIZE_STACK (*last_clauses), sizeof (watch));

  size_t i = 0;

  for (auto c : last_clauses) {
    assert (!c->swept);
    int min_lit = 0;
    unsigned factors = 0;
    size_t min_size = 0;
    ticks++;
    for (const auto &other : *c) {
      if (getfact (other, FACTORS)) {
        if (factors++)
          break;
      } else {
        assert (!getfact (other, QUOTIENT));
        markfact (other, QUOTIENT);
        const size_t other_size = occs (other).size ();
        if (!min_lit || other_size < min_size) {
          min_lit = other;
          min_size = other_size;
        }
      }
    }
    assert (factors);
    if (factors == 1) {
      assert (min_lit);
      const int c_size = c->size;
      ticks += 1; //  + kissat_cache_lines (SIZE_STACK (*min_watches),
                 //                      sizeof (watch));
      for (auto d : occs (min_lit)) {
        if (c == d)
          continue;
        ticks++;
        if (d->swept)
          continue;
        if (d->size != c_size)
          continue;
        for (const auto &other : *d) {
          if (getfact (other, QUOTIENT))
            continue;
          if (other != next)
            goto CONTINUE_WITH_NEXT_MIN_WATCH;
        }
        LOG (c, "matched");
        LOG (d, "keeping");
        
        next_clauses.push_back (d);
        matches.push_back (i);
        flauses.push_back (d);
        d->swept = true;
        break;

      CONTINUE_WITH_NEXT_MIN_WATCH:;
      }
    }
    for (const auto &other : *c)
      if (getfact (other, QUOTIENT))
        unmarkfact (other, QUOTIENT);
    i++;
  }
  clear_flauses (flauses);
  stats.factor_ticks += ticks;

  assert (expected_next_count <= next_clauses.size ());
  (void) expected_next_count;
}

void Internal::resize_factoring (Factoring &factoring, int lit) {
  assert (lit > 0);
  const size_t old_size = factoring.size;
  assert ((size_t) lit >= old_size);
  const size_t old_allocated = factoring.allocated;
  size_t new_var_size = lit + 1;
  size_t new_lit_size = 2 * new_var_size;
  
  // since we have vectors instead of arrays as in kissat this is easy
  // TODO: maybe exponential resize ?
  // or remove allocated...
  if (new_var_size > old_allocated) {
    enlarge_zero (factoring.count, new_lit_size);
    factoring.allocated = new_var_size;
  }
  factoring.size = new_var_size;
}

// TODO: bug in here with preprocesing
void Internal::flush_unmatched_clauses (Quotient *q) {
  Quotient *prev = q->prev;
  vector<size_t> &q_matches = q->matches, &prev_matches = prev->matches;
  vector<Clause *> &q_clauses = q->qlauses, &prev_clauses = prev->qlauses;
  const size_t n = q_clauses.size ();
  assert (n == q_matches.size ());
  bool prev_is_first = !prev->id;
  size_t i = 0;
  while (i < q_matches.size ()) {
    size_t j = q_matches[i];
    assert (i <= j);
    if (!prev_is_first) {
      size_t matches = prev_matches[j];
      prev_matches[i] = matches;
    }
    Clause *c = prev_clauses[j];
    prev_clauses[i] = c;
    i++;
  }
  LOG ("flushing %zu clauses of quotient[%zu]",
       prev_clauses.size () - n, prev->id);
  if (!prev_is_first)
    prev_matches.resize (n);
  prev_clauses.resize (n);
}

void Internal::add_self_subsuming_factor (Quotient *q, Quotient *p) {
  const int factor = q->factor;
  const int not_factor = p->factor;
  assert (-factor == not_factor);
  for (auto c : q->qlauses) {
    for (const auto &lit : *c) {
      if (lit == factor) continue;
      clause.push_back (lit);
    }
    if (lrat) {
      lrat_chain.push_back (c->id);
      for (auto d : p->qlauses) {
        bool match = true;
        for (const auto &lit : *d) {
          if (lit == not_factor) continue;
          if (std::find (clause.begin (), clause.end (), lit) == clause.end ()) {
            match = false;
            break;
          }
        }
        if (match) {
          lrat_chain.push_back (d->id);
          break;
        }
      }
      assert (lrat_chain.size () == 2);
    }
    if (clause.size () > 1) {
      new_factor_clause ();
    } else {
      const int unit = clause[0];
      const signed char tmp = val (unit);
      if (!tmp)
        assign_unit (unit);
      else if (tmp < 0) {
        if (lrat) {
          int64_t id = unit_id (-unit);
          lrat_chain.push_back (id);
          std::reverse (lrat_chain.begin (), lrat_chain.end ());
        }
        learn_empty_clause ();
      }
    }
    clause.clear ();
    lrat_chain.clear ();
  }
}

bool Internal::self_subsuming_factor (Quotient *q) {
  Quotient *x = 0, *y = 0;
  bool found = false;
  for (Quotient *p = q; p; p = p->prev) {
    const int factor = p->factor;
    Flags &f = flags (factor);
    if (f.seen) {
      assert (std::find (analyzed.begin (), analyzed.end (), -factor)
        != analyzed.end ());
      found = true;
      x = p;
      for (Quotient *r = q; r; r = r->prev) {
        if (r->factor != -factor)
          continue;
        y = r;
        break;
      }
      break;
    }
    analyzed.push_back (factor);
    f.seen = true;
  }
  assert (!found || (x && y));
  clear_analyzed_literals ();
  if (found) {
    add_self_subsuming_factor (x, y);
    return true;
  }
  return false;
}

void Internal::add_factored_divider (Quotient *q, int fresh) {
  const int factor = q->factor;
  LOG ("factored %d divider %d", factor, fresh);
  clause.push_back (factor);
  clause.push_back (fresh);
  new_factor_clause ();
  clause.clear ();
  if (lrat)
    mini_chain.push_back (-clause_id);
}

void Internal::blocked_clause (Quotient *q, int not_fresh) {
  if (!proof) return;
  int64_t new_id = ++clause_id;
  q->bid = new_id;
  assert (clause.empty ());
  for (Quotient *p = q; p; p = p->prev)
    clause.push_back (-p->factor);
  clause.push_back (not_fresh);
  assert (!lrat || mini_chain.size ());
  proof->add_derived_clause (new_id, true, clause, mini_chain);
  mini_chain.clear ();
}

void Internal::add_factored_quotient (Quotient *q, int not_fresh) {
  LOG ("adding factored quotient[%zu] clauses", q->id);
  const int factor = q->factor;
  assert (lrat_chain.empty ());
  auto qlauses = q->qlauses;
  for (unsigned idx = 0; idx < qlauses.size (); idx++) {
    const auto c = qlauses[idx];
    for (const auto &other : *c) {
      if (other == factor) {
        continue;
      }
      clause.push_back (other);
    }
    if (lrat) {
      assert (proof);
      assert (q->bid);
      lrat_chain.push_back (q->bid);
      for (Quotient *p = q; p; p = p->prev) {
        lrat_chain.push_back (p->qlauses[idx]->id);
        if (p->prev)
          idx = p->matches[idx];
      }
    }
    clause.push_back (not_fresh);
    new_factor_clause ();
    clause.clear ();
    lrat_chain.clear ();
  }
  if (proof)
    proof->delete_clause (q->bid, true, clause);
}

void Internal::eagerly_remove_from_occurences (Clause *c) {
  for (const auto &lit : *c) {
    auto &occ = occs (lit);
    auto p = occ.begin ();
    auto q = occ.begin ();
    auto begin = occ.begin ();
    auto end = occ.end ();
    while (p != end) {
      *q = *p++;
      if (*q != c) q++;
    }
    assert (q+1 == p);
    occ.resize (q - begin);
  }
}

void Internal::delete_unfactored (Quotient *q) {
  LOG ("deleting unfactored quotient[%zu] clauses", q->id);
  for (auto c : q->qlauses) {
    eagerly_remove_from_occurences (c);
    mark_garbage (c);
    stats.literals_unfactored += c->size;
    stats.clauses_unfactored++;
  }
}


void Internal::update_factored (Factoring &factoring, Quotient *q) {
  const int factor = q->factor;
  update_factor_candidate (factoring, factor);
  update_factor_candidate (factoring, -factor);
  for (auto c : q->qlauses) {
    LOG (c, "deleting unfactored");
    for (const auto &lit : *c)
      if (lit != factor)
        update_factor_candidate (factoring, lit);
  }
}

// TODO: schedule factored variable?
bool Internal::apply_factoring (Factoring &factoring, Quotient *q) {
  for (Quotient *p = q; p->prev; p = p->prev)
    flush_unmatched_clauses (p);
  if (self_subsuming_factor (q)) {
    for (Quotient *p = q; p; p = p->prev)
      delete_unfactored (p);
    for (Quotient *p = q; p; p = p->prev)
      update_factored (factoring, p);
    return true;
  }
  const int fresh = get_new_extension_variable ();
  if (!fresh)
    return false;
  stats.factored++;
  factoring.fresh.push_back (fresh);
  for (Quotient *p = q; p; p = p->prev)
    add_factored_divider (p, fresh);
  const int not_fresh = -fresh;
  blocked_clause (q, not_fresh);
  add_factored_quotient (q, not_fresh);
  for (Quotient *p = q; p; p = p->prev)
    delete_unfactored (p);
  for (Quotient *p = q; p; p = p->prev)
    update_factored (factoring, p);
  assert (fresh > 0);
  resize_factoring (factoring, fresh);
  return true;
}

void Internal::update_factor_candidate (Factoring &factoring, int lit) {
  FactorSchedule &schedule = factoring.schedule;
  const size_t size = occs (lit).size ();
  const unsigned idx = vlit (lit);
  if (schedule.contains (idx))
    schedule.update (idx);
  else if (size > 1) {
    schedule.push_back (idx);
  }
}

void Internal::schedule_factorization (Factoring &factoring) {
  for (const auto &idx : vars) {
    if (active (idx)) {
      Flags &f = flags (idx);
      const int lit = idx;
      const int not_lit = -lit;
      if (f.factor & 1)
        update_factor_candidate (factoring, lit);
      if (f.factor & 2)
        update_factor_candidate (factoring, not_lit);
    }
  }
#ifndef QUIET
  size_t size_cands = factoring.schedule.size ();
  VERBOSE (2, "scheduled %zu factorization candidate literals %.0f %%",
      size_cands, percent (size_cands, max_var));
#endif
}

bool Internal::run_factorization (int64_t limit) {
  Factoring factoring = Factoring (this, limit); // TODO new or not new
  schedule_factorization (factoring);
  bool done = false;
#ifndef QUIET
  unsigned factored = 0;
#endif
  int64_t *ticks = &stats.factor_ticks;
  VERBOSE (3, "factorization limit of %" PRIu64 " ticks", limit - *ticks);

  while (!unsat && !done && !factoring.schedule.empty ()) {
    const unsigned ufirst = factoring.schedule.pop_front ();
    const int first = u2i (ufirst);
    const int first_idx = vidx (first);
    if (!active (first_idx))
      continue;
    if (!occs (first).size ()) {
      factoring.schedule.clear ();
      break;
    }
    if (*ticks > limit) {
      VERBOSE (2, "factorization ticks limit hit");
      break;
    }
    if (terminated_asynchronously ())
      break;
    Flags &f = flags (first_idx);
    const unsigned bit = 1u << (first < 0);
    if (!(f.factor & bit))
      continue;
    f.factor &= ~bit;
    const size_t first_count = first_factor (factoring, first);
    if (first_count > 1) {
      for (;;) {
        unsigned next_count;
        const int next = next_factor (factoring, &next_count);
        if (next == 0)
          break;
        assert (next_count > 1);
        if (next_count < 2)
          break;
        factorize_next (factoring, next, next_count);
      }
      size_t reduction;
      Quotient *q = best_quotient (factoring, &reduction);
      if (q && reduction > factoring.bound) {
        if (apply_factoring (factoring, q)) {
          factored++;
        } else
          done = true;
      }
    }
    release_quotients (factoring);
  }
  
  // since we cannot remove elements from the heap we check wether the
  // first element in the heap has occurences
  bool completed = factoring.schedule.empty ();
  if (!completed) {
    const unsigned idx = factoring.schedule.front ();
    completed = occs (u2i (idx)).empty ();
  }
  // kissat initializes scores for new variables at this point, however
  // this is actually done already during resize of internal
  report ('f', !factored);
  return completed;
}

int Internal::get_new_extension_variable () {
  const int current_max_external = external->max_var;
  const int new_external = current_max_external + 1;
  const int new_internal = external->internalize (new_external, true);
  // one sideeffect of internalize is enlarging the internal datastructures
  // which can initialize the watches (wtab)
  if (watching ()) reset_watches ();
  // it does not enlarge otab, however, so we do this manually
  init_occs ();
  assert (vlit (new_internal));
  return new_internal;
}

void Internal::factor () {
  if (unsat)
    return;
  if (terminated_asynchronously ())
    return;
  if (!opts.factor)
    return;
  assert (stats.mark.factor || clauses.empty ());
  // update last.factor.marked and flags.factor to trigger factor
  if (last.factor.marked >= stats.mark.factor) {
    VERBOSE (3, "factorization skipped as no literals have been"
        "marked to be added (%" PRIu64 " < %" PRIu64 ")",
        last.factor.marked, stats.mark.factor);
    return;
  }
  assert (!level);
  START_SIMPLIFIER (factor, FACTOR);
  stats.factor++;

  int64_t limit = opts.factoriniticks;
  if (stats.factor > 1) {
    int64_t tmp = stats.propagations.search - last.factor.ticks;
    tmp *= opts.factoreffort;
    limit = tmp;
  } else {
    VERBOSE (3, "initially limiting to %" PRIu64
               " million factorization ticks",
               limit);
    limit *= 1e6;
    limit += stats.factor_ticks;
  }

  // TODO commented code for messages
  /*
  struct {
    int64_t variables, clauses, ticks;
  } before, after, delta;
  before.variables = stats.variables_extension + stats.variables_original;
  before.ticks = stats.factor_ticks;
  */
  factor_mode ();
  bool completed = run_factorization (limit);
  reset_factor_mode ();

  /*
  after.variables = s->variables_extension + s->variables_original;
  after.binary = BINARY_CLAUSES;
  after.clauses = IRREDUNDANT_CLAUSES;
  after.ticks = s->factor_ticks;
  delta.variables = after.variables - before.variables;
  delta.binary = before.binary - after.binary;
  delta.clauses = before.clauses - after.clauses;
  delta.ticks = after.ticks - before.ticks;
  kissat_very_verbose (solver, "used %f million factorization ticks",
                       delta.ticks * 1e-6);
  kissat_phase (solver, "factorization", GET (factorizations),
                "introduced %" PRId64 " extension variables %.0f%%",
                delta.variables,
                kissat_percent (delta.variables, before.variables));
  kissat_phase (solver, "factorization", GET (factorizations),
                "removed %" PRId64 " binary clauses %.0f%%", delta.binary,
                kissat_percent (delta.binary, before.binary));
  kissat_phase (solver, "factorization", GET (factorizations),
                "removed %" PRId64 " large clauses %.0f%%", delta.clauses,
                kissat_percent (delta.clauses, before.clauses));

  VERBOSE (2, "factored %" PRIu64 " new variables", factored);
  VERBOSE (2, "factorization added %" PRIu64 " and deleted %" PRIu64 " clauses", added, deleted);
  */
  if (completed)
    last.factor.marked = stats.mark.factor;
  STOP_SIMPLIFIER (factor, FACTOR);
}

}