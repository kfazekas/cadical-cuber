#include "cadical.hpp"
#include <iostream>
#include <cstring>

int main (int argc, char ** argv) {
    const char * input_path = 0;
    const char * output_path = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp (argv[i], "-i")) {
            if (i < argc - 1) input_path = argv[++i];
            else {
                std::cerr << "Option -i should be followed by an input file" << std::endl;
                return 1;
            }
        }
        if (!strcmp (argv[i], "-o")) {
            if (i < argc - 1) output_path = argv[++i];
            else {
                std::cerr << "Option -o should be followed by a path to save output to." << std::endl;
                return 1;
            }
        }
    }
    
    if (!input_path || !output_path) {
        std::cerr << "Usage: ./simp -i input-cnf-file -o output-path" << std::endl;
        return 1;
    }

    FILE * tmp = fopen (input_path, "r");
    
    if (!tmp) {
        std::cerr << "Could not open file: " << input_path << std::endl;
        return 1;
    } else {
        std::cout << "Input: " << input_path << std::endl;
        fclose(tmp);
    }

    tmp = fopen (output_path, "w");
    if (!tmp) {
        std::cerr << "Could not open file: " << output_path << std::endl;
        return 1;
    } else {
        std::cout << "Output: " << output_path << std::endl;
        fclose(tmp);
    }

    CaDiCaL::Solver * cadical = new CaDiCaL::Solver();
    int vars = 0;
    cadical->read_dimacs(input_path,vars);

    cadical->simplify ();
    cadical->statistics ();
    cadical->write_dimacs(output_path);
    
    if (cadical) delete cadical;
    return 0;
}