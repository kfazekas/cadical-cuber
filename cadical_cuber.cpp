#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <iostream>
#include <iomanip>
#include <signal.h>
#include <map>

#include "cadical.hpp"
#include "file.hpp"

#include <fstream>
#include <cstring>


const float ver {1.0};

static void SIGINT_exit(int);

static void (*signal_SIGINT)(int);
static void (*signal_SIGXCPU)(int);
static void (*signal_SIGSEGV)(int);
static void (*signal_SIGTERM)(int);
static void (*signal_SIGABRT)(int);

static void SIGINT_exit(int signum) {
  signal(SIGINT, signal_SIGINT);
  signal(SIGXCPU, signal_SIGXCPU);
  signal(SIGSEGV, signal_SIGSEGV);
  signal(SIGTERM, signal_SIGTERM);
  signal(SIGABRT, signal_SIGABRT);

  std::cout << "c Signal interruption." << std::endl;

  fflush(stdout);
  fflush(stderr);

  raise (signum);
}


int get_number_arg(std::string const& arg) {
  try {
    std::size_t pos;
    int x = std::stoi(arg, &pos);
    if (pos < arg.size()) {
      std::cerr << "Trailing characters after number: " << arg << '\n';
    }
    return x;
  } catch (std::invalid_argument const &ex) {
    std::cerr << "Invalid number: " << arg << '\n';
    return 0;
  } catch (std::out_of_range const &ex) {
    std::cerr << "Number out of range: " << arg << '\n';
    return 0;
  }
}   

void print_usage() {
  std::cout << "usage: cadical_cuber <path to dimacs> <depth> <path to cubes dimacs> <cubing-strategy>, where cubing strategy can be" << std::endl;
  std::cout << "\t 1: cubing based on dynamic occurrence count [default]" << std::endl;
  std::cout << "\t 2: cubing based on TreeLook algorithm" << std::endl;
  std::cout << "\t 3: cubing based on sum of number of propagations" << std::endl;
}

int d = 0;
const char * cubes_path = 0;
const char * dimacs_path = 0;

std::vector<int> fixed_collection;

class FixCollector : public CaDiCaL::FixedAssignmentListener {
  void notify_fixed_assignment (int lit) {
    fixed_collection.push_back(lit);
    std::cout << "New fixed lit: " << lit << std::endl;
  }
};

int main (int argc, char ** argv) {
  signal_SIGINT =  signal(SIGINT, SIGINT_exit);
  signal_SIGXCPU = signal(SIGXCPU, SIGINT_exit);
  signal_SIGSEGV = signal(SIGSEGV, SIGINT_exit);
  signal_SIGTERM = signal(SIGTERM, SIGINT_exit);
  signal_SIGABRT = signal(SIGABRT, SIGINT_exit);

  std::string dimacs_file;
  if (argc > 1 && argv[1] == std::string("--help")) {
    print_usage ();
    return 0;
  }
  if(argc < 4) {
    std::cerr << "c Error, missing input arguments." << std::endl;
    print_usage();
    return 1;
  }
  
  dimacs_path = argv[1];
  d = get_number_arg (argv[2]);
  cubes_path = argv[3];
  int cubing_strategy = 1;
  if (argc > 4) {
    int tmp = get_number_arg (argv[4]);
    if (tmp <= 0 || tmp > 3) {
      std::cerr << "Error, invalid cubing strategy." << std::endl;
      print_usage();
      return 1;
    }
    cubing_strategy = tmp;
  }
  std::cout << "c Used cubing strategy: " << cubing_strategy << std::endl;
  CaDiCaL::Solver * _cadical = new CaDiCaL::Solver();
  _cadical->set("log",0);

  // FixCollector* fal = new FixCollector ();
  // _cadical->connect_fixed_listener(fal);

  int max_var = 0;
  const char* err = _cadical->read_dimacs(dimacs_path,max_var);
  if (err) {
    std::cerr << "Error while reading input: " << err << std::endl;
    return 1;
  }
  
  std::cout << "c Maximum variable: " << max_var << std::endl;
  if (d > max_var) d = max_var;

  CaDiCaL::Solver::CubesWithStatus cs = _cadical->generate_dynamic_cubes(d,cubing_strategy);


  if (!CaDiCaL::File::writable (cubes_path)) {
    std::cerr << "Error, cannot write cubes to: " << cubes_path << std::endl;
    return 1;
  }
  std::ofstream cube_dimacs(cubes_path);
  cube_dimacs << "c This is a DNF formula, but p dnf is not accepted by SDD." << std::endl;
  cube_dimacs << "c input: " << dimacs_path << std::endl;
  cube_dimacs << "c depth: " << d << std::endl;
  cube_dimacs << "c cubing strategy: " << cubing_strategy << std::endl;
  cube_dimacs << "p cnf " << max_var << " " << cs.cubes.size() << std::endl;
  
  for (const auto &cube : cs.cubes) {
    if (!cube.size()) continue;
    for (const auto &lit : cube) {
      cube_dimacs << lit << " ";
    }
    cube_dimacs << "0" << std::endl;
  }

  cube_dimacs.close();

  delete _cadical;
  return 0;
}