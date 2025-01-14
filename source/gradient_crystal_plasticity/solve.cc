#include <gCP/gradient_crystal_plasticity.h>

#include <deal.II/numerics/data_out.h>

namespace gCP
{



  template <int dim>
  void GradientCrystalPlasticitySolver<dim>::extrapolate_initial_trial_solution()
  {
    dealii::LinearAlgebraTrilinos::MPI::Vector distributed_trial_solution;

    dealii::LinearAlgebraTrilinos::MPI::Vector distributed_old_solution;

    dealii::LinearAlgebraTrilinos::MPI::Vector distributed_newton_update;

    distributed_trial_solution.reinit(fe_field->distributed_vector);

    distributed_old_solution.reinit(fe_field->distributed_vector);

    distributed_newton_update.reinit(fe_field->distributed_vector);

    distributed_trial_solution  = fe_field->old_solution;

    distributed_old_solution    = fe_field->old_old_solution;

    distributed_newton_update   = fe_field->old_solution;

    bool flag_extrapolate_old_solutions = true;

    if (temporal_discretization_parameters.loading_type ==
          RunTimeParameters::LoadingType::Cyclic ||
        temporal_discretization_parameters.loading_type ==
          RunTimeParameters::LoadingType::CyclicWithUnloading)
    {
      const bool maximum_of_preloading_phase =
          discrete_time.get_step_number() ==
            temporal_discretization_parameters.n_steps_in_preloading_phase / 2;

      const bool start_of_loading_phase =
          discrete_time.get_step_number() ==
            temporal_discretization_parameters.n_steps_in_preloading_phase;

      const bool start_of_cyclic_phase =
          discrete_time.get_step_number() ==
            temporal_discretization_parameters.n_steps_in_preloading_phase +
            temporal_discretization_parameters.n_steps_in_loading_and_unloading_phases;

      const bool start_of_unloading_phase =
          discrete_time.get_step_number() ==
            temporal_discretization_parameters.n_steps_in_preloading_phase +
            temporal_discretization_parameters.n_steps_in_loading_and_unloading_phases +
            temporal_discretization_parameters.n_steps_per_half_cycle * 2.0 *
              temporal_discretization_parameters.n_cycles;

      const int n_cycles =
        std::floor(
          (discrete_time.get_current_time() -
           temporal_discretization_parameters.start_of_cyclic_phase) /
          temporal_discretization_parameters.period);

      const unsigned int effective_step_number =
        std::abs(
        discrete_time.get_step_number() -
        temporal_discretization_parameters.n_steps_in_preloading_phase -
        temporal_discretization_parameters.n_steps_in_loading_and_unloading_phases -
        temporal_discretization_parameters.n_steps_per_half_cycle * 2.0 * n_cycles);

      bool extrema_step_of_cyclic_phase =
        effective_step_number ==
          (2 * temporal_discretization_parameters.n_steps_per_half_cycle * 1 / 4) ||
        effective_step_number ==
          (2 * temporal_discretization_parameters.n_steps_per_half_cycle * 3 / 4);

      if (n_cycles < 0 ||
          (discrete_time.get_step_number() >
            temporal_discretization_parameters.n_steps_in_preloading_phase +
            temporal_discretization_parameters.n_steps_in_loading_and_unloading_phases +
            temporal_discretization_parameters.n_steps_per_half_cycle * 2.0 *
              temporal_discretization_parameters.n_cycles))
      {
        extrema_step_of_cyclic_phase = false;
      }

      if ((maximum_of_preloading_phase ||
           start_of_loading_phase ||
           start_of_cyclic_phase ||
           start_of_unloading_phase ||
           extrema_step_of_cyclic_phase) &&
          parameters.flag_skip_extrapolation_at_extrema)
      {
        //std::cout << "Keine Extrapolation" << std::endl;
        flag_extrapolate_old_solutions = false;
      }
    }

    double step_size_ratio = 1.0;

    if (discrete_time.get_step_number() > 0)
    {
      step_size_ratio =
        discrete_time.get_next_step_size() /
        discrete_time.get_previous_step_size();
    }

    //flag_extrapolate_old_solutions = false;

    if (flag_extrapolate_old_solutions)
    {
      distributed_trial_solution.sadd(
        1.0 + step_size_ratio,
        -step_size_ratio,
        distributed_old_solution);

      distributed_newton_update.sadd(
        -1.0,
        1.0,
        distributed_trial_solution);
    }

    fe_field->get_affine_constraints().distribute(
      distributed_trial_solution);

    fe_field->get_newton_method_constraints().distribute(
      distributed_newton_update);

    trial_solution  = distributed_trial_solution;

    newton_update   = distributed_newton_update;
  }



  template <int dim>
  std::tuple<bool, unsigned int> GradientCrystalPlasticitySolver<dim>::solve_nonlinear_system()
  {
    nonlinear_solver_logger.add_break(
      "Step " + std::to_string(discrete_time.get_step_number() + 1) +
      ": Solving for t = " + std::to_string(discrete_time.get_next_time()) +
      " with dt = " + std::to_string(discrete_time.get_next_step_size()));

    nonlinear_solver_logger.log_headers_to_terminal();

    extrapolate_initial_trial_solution();

    //return (std::make_tuple(true,0));

    store_trial_solution(true);

    prepare_quadrature_point_history();

    bool flag_successful_convergence      = false;

    unsigned int nonlinear_iteration      = 0;

    double previous_residual_norm         = 0.0;

    const RunTimeParameters::NewtonRaphsonParameters
      &newton_parameters = parameters.newton_parameters;

    // Newton-Raphson loop
    do
    {
      nonlinear_iteration++;

      AssertThrow(
        nonlinear_iteration <= newton_parameters.n_max_iterations,
        dealii::ExcMessage(
          "The nonlinear solver has reach the given maximum number of "
          "iterations (" +
          std::to_string(newton_parameters.n_max_iterations) + ")."));

      // The current trial solution has to be stored in case
      store_trial_solution();

      reset_and_update_quadrature_point_history();

      const double initial_value_scalar_function = assemble_residual();

      if (nonlinear_iteration == 1)
      {
        const auto residual_l2_norms =
            fe_field->get_l2_norms(residual);

        previous_residual_norm = residual_norm;

        nonlinear_solver_logger.update_value("N-Itr",
                                             0);
        nonlinear_solver_logger.update_value("K-Itr",
                                             0.0);
        nonlinear_solver_logger.update_value("L-Itr",
                                             0.0);
        nonlinear_solver_logger.update_value("(NS)_L2",
                                             0.0);
        nonlinear_solver_logger.update_value("(NS_U)_L2",
                                             0.0);
        nonlinear_solver_logger.update_value("(NS_G)_L2",
                                             0.0);
        nonlinear_solver_logger.update_value("(R)_L2",
                                             std::get<0>(residual_l2_norms));
        nonlinear_solver_logger.update_value("(R_U)_L2",
                                             std::get<1>(residual_l2_norms));
        nonlinear_solver_logger.update_value("(R_G)_L2",
                                             std::get<2>(residual_l2_norms));
        nonlinear_solver_logger.update_value("C-Rate",
                                             0.);

        nonlinear_solver_logger.log_to_file();

        nonlinear_solver_logger.log_values_to_terminal();
      }

      assemble_jacobian();

      const unsigned int n_krylov_iterations = solve_linearized_system();

      double relaxation_parameter = 1.0;

      update_trial_solution(relaxation_parameter);

      reset_and_update_quadrature_point_history();

      // Line search algorithm
      {
        double trial_value_scalar_function = assemble_residual();

        line_search.reinit(initial_value_scalar_function);

        while (!line_search.suficient_descent_condition(
            trial_value_scalar_function, relaxation_parameter))
        {
          relaxation_parameter =
              line_search.get_lambda(trial_value_scalar_function,
                                     relaxation_parameter);

          reset_trial_solution();

          update_trial_solution(relaxation_parameter);

          reset_and_update_quadrature_point_history();

          trial_value_scalar_function = assemble_residual();
        }
      }

      // Terminal and log output
      {
        const auto residual_l2_norms =
            fe_field->get_l2_norms(residual);

        const auto newton_update_l2_norms =
            fe_field->get_l2_norms(newton_update);

        const double order_of_convergence =
            (nonlinear_iteration > 1) ? std::log(residual_norm) /
              std::log(previous_residual_norm) : 0.0;

        previous_residual_norm = residual_norm;

        nonlinear_solver_logger.update_value("N-Itr",
                                            nonlinear_iteration);
        nonlinear_solver_logger.update_value("K-Itr",
                                            n_krylov_iterations);
        nonlinear_solver_logger.update_value("L-Itr",
                                            line_search.get_n_iterations());
        nonlinear_solver_logger.update_value("(NS)_L2",
                                            relaxation_parameter *
                                            std::get<0>(newton_update_l2_norms));
        nonlinear_solver_logger.update_value("(NS_U)_L2",
                                            relaxation_parameter *
                                            std::get<1>(newton_update_l2_norms));
        nonlinear_solver_logger.update_value("(NS_G)_L2",
                                            relaxation_parameter *
                                            std::get<2>(newton_update_l2_norms));
        nonlinear_solver_logger.update_value("(R)_L2",
                                            std::get<0>(residual_l2_norms));
        nonlinear_solver_logger.update_value("(R_U)_L2",
                                            std::get<1>(residual_l2_norms));
        nonlinear_solver_logger.update_value("(R_G)_L2",
                                            std::get<2>(residual_l2_norms));
        nonlinear_solver_logger.update_value("C-Rate",
                                            order_of_convergence);

        nonlinear_solver_logger.log_to_file();

        nonlinear_solver_logger.log_values_to_terminal();
      }

      //slip_rate_output(true);

      flag_successful_convergence =
          residual_norm < newton_parameters.absolute_tolerance ||
          ((relaxation_parameter * newton_update_norm) <
            newton_parameters.step_tolerance &&
            residual_norm < 100. * newton_parameters.absolute_tolerance);

      if ((relaxation_parameter * newton_update_norm) <
            newton_parameters.step_tolerance &&
            residual_norm > 100. * newton_parameters.absolute_tolerance)
      {
        AssertThrow(
          false,
          dealii::ExcMessage(
            "The Newton step became too small but the residual has not "
            "reached an acceptable value."));
      }

    } while (!flag_successful_convergence);

    //slip_rate_output(false);

    print_out = true;

    store_effective_opening_displacement_in_quadrature_history();

    fe_field->solution = trial_solution;

    *pcout << std::endl;

    return (std::make_tuple(flag_successful_convergence, nonlinear_iteration));
  }



  template <int dim>
  unsigned int GradientCrystalPlasticitySolver<dim>::solve_linearized_system()
  {
    if (parameters.verbose)
      *pcout << std::setw(38) << std::left
             << "  Solver: Solving linearized system...";

    dealii::TimerOutput::Scope t(*timer_output, "Solver: Solve ");

    // In this method we create temporal non ghosted copies
    // of the pertinent vectors to be able to perform the solve()
    // operation.
    dealii::LinearAlgebraTrilinos::MPI::Vector distributed_newton_update;

    distributed_newton_update.reinit(fe_field->distributed_vector);

    distributed_newton_update = newton_update;

    const RunTimeParameters::KrylovParameters &krylov_parameters =
      parameters.krylov_parameters;

    // The solver's tolerances are passed to the SolverControl instance
    // used to initialize the solver
    dealii::SolverControl solver_control(
        krylov_parameters.n_max_iterations,
        std::max(residual_norm * krylov_parameters.relative_tolerance,
                 krylov_parameters.absolute_tolerance));

    switch (krylov_parameters.solver_type)
    {
      case RunTimeParameters::SolverType::DirectSolver:
      {
        dealii::TrilinosWrappers::SolverDirect solver(solver_control);

        try
        {
          solver.solve(jacobian, distributed_newton_update, residual);
        }
        catch (std::exception &exc)
        {
          std::cerr << std::endl
                    << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::cerr << "Exception in the solve method: " << std::endl
                    << exc.what() << std::endl
                    << "Aborting!" << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::abort();
        }
        catch (...)
        {
          std::cerr << std::endl
                    << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::cerr << "Unknown exception in the solve method!" << std::endl
                    << "Aborting!" << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::abort();
        }
      }
      break;

    case RunTimeParameters::SolverType::CG:
      {
        dealii::LinearAlgebraTrilinos::SolverCG solver(solver_control);

        dealii::LinearAlgebraTrilinos::MPI::PreconditionILU::AdditionalData
          additional_data;

        dealii::LinearAlgebraTrilinos::MPI::PreconditionILU preconditioner;

        preconditioner.initialize(jacobian, additional_data);

        try
        {
          solver.solve(jacobian,
                      distributed_newton_update,
                      residual,
                      preconditioner);
        }
        catch (std::exception &exc)
        {
          std::cerr << std::endl
                    << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::cerr << "Exception in the solve method: " << std::endl
                    << exc.what() << std::endl
                    << "Aborting!" << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::abort();
        }
        catch (...)
        {
          std::cerr << std::endl
                    << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::cerr << "Unknown exception in the solve method!" << std::endl
                    << "Aborting!" << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::abort();
        }
      }
      break;

    case RunTimeParameters::SolverType::GMRES:
      {
        dealii::LinearAlgebraTrilinos::SolverGMRES solver(solver_control);

        dealii::LinearAlgebraTrilinos::MPI::PreconditionILU preconditioner;

        preconditioner.initialize(jacobian);

        try
        {
          solver.solve(jacobian,
                      distributed_newton_update,
                      residual,
                      preconditioner);
        }
        catch (std::exception &exc)
        {
          std::cerr << std::endl
                    << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::cerr << "Exception in the solve method: " << std::endl
                    << exc.what() << std::endl
                    << "Aborting!" << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::abort();
        }
        catch (...)
        {
          std::cerr << std::endl
                    << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::cerr << "Unknown exception in the solve method!" << std::endl
                    << "Aborting!" << std::endl
                    << "----------------------------------------------------"
                    << std::endl;
          std::abort();
        }
      }
      break;

    default:
      AssertThrow(false, dealii::ExcNotImplemented());
      break;
    }

    // Zero out the Dirichlet boundary conditions
    fe_field->get_newton_method_constraints().distribute(
        distributed_newton_update);

    // Pass the distributed vectors to their ghosted counterpart
    newton_update = distributed_newton_update;

    // Compute the L2-Norm of the Newton update
    newton_update_norm = distributed_newton_update.l2_norm();

    if (parameters.verbose)
      *pcout << " done!" << std::endl;

    return (solver_control.last_step());
  }



  template <int dim>
  void GradientCrystalPlasticitySolver<dim>::update_trial_solution(
      const double relaxation_parameter)
  {
    dealii::LinearAlgebraTrilinos::MPI::Vector distributed_trial_solution;
    dealii::LinearAlgebraTrilinos::MPI::Vector distributed_newton_update;

    distributed_trial_solution.reinit(fe_field->distributed_vector);
    distributed_newton_update.reinit(fe_field->distributed_vector);

    distributed_trial_solution  = trial_solution;
    distributed_newton_update   = newton_update;

    distributed_trial_solution.add(relaxation_parameter, distributed_newton_update);

    fe_field->get_affine_constraints().distribute(distributed_trial_solution);

    trial_solution = distributed_trial_solution;
  }



  template <int dim>
  void GradientCrystalPlasticitySolver<dim>::store_trial_solution(
    const bool flag_store_initial_trial_solution)
  {
    dealii::LinearAlgebraTrilinos::MPI::Vector distributed_trial_solution;

    distributed_trial_solution.reinit(fe_field->distributed_vector);

    distributed_trial_solution = trial_solution;

    fe_field->get_affine_constraints().distribute(distributed_trial_solution);

    if (flag_store_initial_trial_solution)
    {
      initial_trial_solution  = distributed_trial_solution;
    }
    else
    {
      tmp_trial_solution      = distributed_trial_solution;
    }
  }



  template <int dim>
  void GradientCrystalPlasticitySolver<dim>::reset_trial_solution(
    const bool flag_reset_to_initial_trial_solution)
  {
    dealii::LinearAlgebraTrilinos::MPI::Vector distributed_trial_solution;

    distributed_trial_solution.reinit(fe_field->distributed_vector);

    if (flag_reset_to_initial_trial_solution)
    {
      distributed_trial_solution = fe_field->old_solution; // initial_trial_solution
    }
    else
    {
      distributed_trial_solution = tmp_trial_solution;
    }

    fe_field->get_affine_constraints().distribute(distributed_trial_solution);

    trial_solution = distributed_trial_solution;
  }



  template <int dim>
  bool GradientCrystalPlasticitySolver<dim>::compute_initial_guess()
  {
    bool flag_successful_convergence = false;

    unsigned int nonlinear_iteration = 0;

    double previous_residual_norm = 0.0;

    const RunTimeParameters::NewtonRaphsonParameters
      &newton_parameters = parameters.newton_parameters;

    // Newton-Raphson loop
    do
    {
      nonlinear_iteration++;

      /*AssertThrow(
          nonlinear_iteration <= newton_parameters.n_max_iterations,
          dealii::ExcMessage("The nonlinear solver has reach the given "
                             "maximum number of iterations (" +
                             std::to_string(newton_parameters.n_max_iterations) + ")."));
      */

      if (nonlinear_iteration > newton_parameters.n_max_iterations)
      {
        std::stringstream message;

        message << "\n  Maximum amount of nonlinear iterations reached. "
                << "Computing a new initial solution with";

        nonlinear_solver_logger.add_break(message.str().c_str());

        *pcout << message.rdbuf();

        return (false);
      }

      store_trial_solution();

      reset_and_update_quadrature_point_history();

      const double initial_value_scalar_function = assemble_residual();

      assemble_jacobian();

      const unsigned int n_krylov_iterations = solve_linearized_system();

      double relaxation_parameter = 1.0;

      update_trial_solution(relaxation_parameter);

      reset_and_update_quadrature_point_history();

      // Line search algorithm
      {
        double trial_value_scalar_function = assemble_residual();

        line_search.reinit(initial_value_scalar_function);

        while (!line_search.suficient_descent_condition(
            trial_value_scalar_function, relaxation_parameter))
        {
          relaxation_parameter =
              line_search.get_lambda(trial_value_scalar_function, relaxation_parameter);

          reset_trial_solution();

          update_trial_solution(relaxation_parameter);

          reset_and_update_quadrature_point_history();

          trial_value_scalar_function = assemble_residual();
        }
      }

      // Terminal and log output
      {
        const auto residual_l2_norms =
            fe_field->get_l2_norms(residual);

        const auto newton_update_l2_norms =
            fe_field->get_l2_norms(newton_update);

        const double order_of_convergence =
            (nonlinear_iteration > 1) ? std::log(residual_norm) / std::log(previous_residual_norm) : 0.0;

        previous_residual_norm = residual_norm;

        nonlinear_solver_logger.update_value("N-Itr",
                                            nonlinear_iteration);
        nonlinear_solver_logger.update_value("K-Itr",
                                            n_krylov_iterations);
        nonlinear_solver_logger.update_value("L-Itr",
                                            line_search.get_n_iterations());
        nonlinear_solver_logger.update_value("(NS)_L2",
                                            std::get<0>(newton_update_l2_norms));
        nonlinear_solver_logger.update_value("(NS_U)_L2",
                                            std::get<1>(newton_update_l2_norms));
        nonlinear_solver_logger.update_value("(NS_G)_L2",
                                            std::get<2>(newton_update_l2_norms));
        nonlinear_solver_logger.update_value("(R)_L2",
                                            std::get<0>(residual_l2_norms));
        nonlinear_solver_logger.update_value("(R_U)_L2",
                                            std::get<1>(residual_l2_norms));
        nonlinear_solver_logger.update_value("(R_G)_L2",
                                            std::get<2>(residual_l2_norms));
        nonlinear_solver_logger.update_value("C-Rate",
                                            order_of_convergence);

        nonlinear_solver_logger.log_to_file();

        nonlinear_solver_logger.log_values_to_terminal();
      }

      flag_successful_convergence =
          residual_norm < newton_parameters.absolute_tolerance ||
          newton_update_norm < newton_parameters.step_tolerance;

    } while (!flag_successful_convergence);

    return (true);
  }


} // namespace gCP



template void gCP::GradientCrystalPlasticitySolver<2>::extrapolate_initial_trial_solution();
template void gCP::GradientCrystalPlasticitySolver<3>::extrapolate_initial_trial_solution();

template std::tuple<bool,unsigned int> gCP::GradientCrystalPlasticitySolver<2>::solve_nonlinear_system();
template std::tuple<bool,unsigned int> gCP::GradientCrystalPlasticitySolver<3>::solve_nonlinear_system();

template unsigned int gCP::GradientCrystalPlasticitySolver<2>::solve_linearized_system();
template unsigned int gCP::GradientCrystalPlasticitySolver<3>::solve_linearized_system();

template void gCP::GradientCrystalPlasticitySolver<2>::update_trial_solution(const double);
template void gCP::GradientCrystalPlasticitySolver<3>::update_trial_solution(const double);
