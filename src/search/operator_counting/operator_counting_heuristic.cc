#include "operator_counting_heuristic.h"

#include <cmath>

#include "../option_parser.h"
#include "../plugin.h"
#include "../utils/markup.h"
#include "constraint_generator.h"

using namespace std;

namespace operator_counting {
OperatorCountingHeuristic::OperatorCountingHeuristic(const Options &opts)
    : Heuristic(opts),
      constraint_generators(
          opts.get_list<shared_ptr<ConstraintGenerator>>("constraint_generators")),
      lp_solver(opts.get<lp::LPSolverType>("lpsolver")),
      cache_lp(opts.get<bool>("cache_lp")),
      use_integer_operator_counts(opts.get<bool>("use_integer_operator_counts")),
      debug_cache(opts.get<bool>("debug_cache")),
      evaluations(0),
      cache_hits(0) {
    lp_solver.set_mip_gap(0);
    named_vector::NamedVector<lp::LPVariable> variables;
    double infinity = lp_solver.get_infinity();
    for (OperatorProxy op : task_proxy.get_operators()) {
        int op_cost = op.get_cost();
        variables.push_back(lp::LPVariable(0, infinity, op_cost, use_integer_operator_counts));
    }
    lp::LinearProgram lp(lp::LPObjectiveSense::MINIMIZE, move(variables), {}, infinity);
    for (const auto &generator : constraint_generators) {
        generator->initialize_constraints(task, lp);
    }
    lp_solver.load_problem(lp);
    if (cache_lp && constraint_generators.size() > 1) {
        cerr << "lp_caching is not supported for more than ONE constraint generator" << endl;
        utils::exit_with(utils::ExitCode::SEARCH_INPUT_ERROR);
    }
}

OperatorCountingHeuristic::~OperatorCountingHeuristic() {
    cout << "cache hits: " << cache_hits << endl;
    assert(cache_hits <= evaluations);
}

int OperatorCountingHeuristic::compute_heuristic(const State &ancestor_state) {
    assert(ancestor_state.get_id() != StateID::no_state);
    State state = convert_ancestor_state(ancestor_state);
    int h_cache = NO_VALUE;
    evaluations++;
    for (auto &generator:constraint_generators) {
        generator->set_active_state(state);
    }
    if (cache_lp) {
        //cout << "get cache" << endl;
        h_cache = constraint_generators[0]->get_cached_heuristic_value(state);
        if (h_cache != NO_VALUE) {
            ++cache_hits;
        }
    }
    double result = NO_VALUE;
    if (h_cache == NO_VALUE || debug_cache) {
        assert(!lp_solver.has_temporary_constraints());
        for (const auto &generator : constraint_generators) {
            bool dead_end = generator->update_constraints(state, lp_solver);
            if (dead_end) {
                result = DEAD_END;
                //cout << "dead end" << endl;
            }
        }
        if (result == NO_VALUE) {
            lp_solver.solve();
            if (lp_solver.has_optimal_solution()) {
                //lp_solver.write_lp("debug-" + std::to_string(evaluations));
                result = lp_solver.get_objective_value();
                //cout << "solved lp: " << result << endl;
            } else {
                //cout << "dead end type 2" << endl;
                result = DEAD_END;
            }
        }
        if (cache_lp && h_cache == NO_VALUE) {
            constraint_generators[0]->cache_heuristic(state, lp_solver, result);
        }
        //cout << "cache: " << h_cache << endl;
        //cout << "lp: " << result << ":" << ceil(result - 0.01) << endl;
        //debug_cache -> X
        assert(!debug_cache || (h_cache == NO_VALUE || h_cache == ceil(result - 0.01)));
    } else {
        result = h_cache;
    }
    assert(result != NO_VALUE);
    lp_solver.clear_temporary_constraints();
    return ceil(result - 0.01);
}

static shared_ptr<Heuristic> _parse(OptionParser &parser) {
    parser.document_synopsis(
        "Operator-counting heuristic",
        "An operator-counting heuristic computes a linear program (LP) in each "
        "state. The LP has one variable Count_o for each operator o that "
        "represents how often the operator is used in a plan. Operator-"
        "counting constraints are linear constraints over these varaibles that "
        "are guaranteed to have a solution with Count_o = occurrences(o, pi) "
        "for every plan pi. Minimizing the total cost of operators subject to "
        "some operator-counting constraints is an admissible heuristic. "
        "For details, see" +
        utils::format_conference_reference(
            {"Florian Pommerening", "Gabriele Roeger", "Malte Helmert",
             "Blai Bonet"},
            "LP-based Heuristics for Cost-optimal Planning",
            "http://www.aaai.org/ocs/index.php/ICAPS/ICAPS14/paper/view/7892/8031",
            "Proceedings of the Twenty-Fourth International Conference"
            " on Automated Planning and Scheduling (ICAPS 2014)",
            "226-234",
            "AAAI Press",
            "2014"));

    parser.document_language_support("action costs", "supported");
    parser.document_language_support(
        "conditional effects",
        "not supported (the heuristic supports them in theory, but none of "
        "the currently implemented constraint generators do)");
    parser.document_language_support(
        "axioms",
        "not supported (the heuristic supports them in theory, but none of "
        "the currently implemented constraint generators do)");
    parser.document_property("admissible", "yes");
    parser.document_property(
        "consistent",
        "yes, if all constraint generators represent consistent heuristics");
    parser.document_property("safe", "yes");
    // TODO: prefer operators that are non-zero in the solution.
    parser.document_property("preferred operators", "no");

    parser.add_list_option<shared_ptr<ConstraintGenerator>>(
        "constraint_generators",
        "methods that generate constraints over operator-counting variables");

    parser.add_option<bool>(
        "cache_lp",
        "try to avoid lp computations by caching previous solutions", "false");
    parser.add_option<bool>(
        "debug_cache",
        "compute lps although caching is enabled and check if the cached predictions are correct",
        "false");

    parser.add_option<bool>(
        "use_integer_operator_counts",
        "restrict operator-counting variables to integer values. Computing the "
        "heuristic with integer variables can produce higher values but "
        "requires solving a MIP instead of an LP which is generally more "
        "computationally expensive. Turning this option on can thus drastically "
        "increase the runtime.",
        "false");

    lp::add_lp_solver_option_to_parser(parser);
    Heuristic::add_options_to_parser(parser);
    Options opts = parser.parse();
    if (parser.help_mode())
        return nullptr;
    opts.verify_list_non_empty<shared_ptr<ConstraintGenerator>>(
        "constraint_generators");
    if (parser.dry_run())
        return nullptr;
    return make_shared<OperatorCountingHeuristic>(opts);
}

static Plugin<Evaluator> _plugin("operatorcounting", _parse);
}
