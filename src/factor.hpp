#ifndef _factor_hpp_INCLUDED
#define _factor_hpp_INCLUDED

#include "heap.hpp"

namespace CaDiCaL {

struct Internal;


struct factor_occs_size {
  Internal *internal;
  factor_occs_size (Internal *i) : internal (i) {}
  bool operator() (unsigned a, unsigned b);
};

struct Quotient {
  size_t id;
  Quotient *prev, *next;
  unsigned factor;
  // statches clauses;  // TODO statches?
  vector<size_t> matches;  // TODO sizes type correct?
  size_t matched;
};

struct Scores {
  double *score;
  vector<unsigned> scored;
};

typedef heap<factor_occs_size> FactorSchedule;

struct Factoring {
  Factoring (Internal *, int64_t);
  ~Factoring ();

  // These are initialized by the constructor
  Internal *internal;
  int64_t limit;
  FactorSchedule schedule;

  size_t size, allocated;
  unsigned initial;
  Scores *scores;
  unsigned bound;
  vector<unsigned> count;
  vector<int> fresh;
  vector<unsigned> counted;
  vector<unsigned> nounted;
  vector<Clause *> qlauses;
  struct {
    Quotient *first, *last;
  } quotients;
};

} // namespace CaDiCaL

#endif
