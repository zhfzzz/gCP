#include <gCP/assembly_data.h>
#include <gCP/gradient_crystal_plasticity.h>

#include <deal.II/base/work_stream.h>
#include <deal.II/grid/filtered_iterator.h>

namespace gCP
{



template <int dim>
void GradientCrystalPlasticitySolver<dim>::assemble_jacobian()
{
  if (parameters.verbose)
    *pcout << std::setw(38) << std::left
           << "  Solver: Assembling jacobian...";

  dealii::TimerOutput::Scope  t(*timer_output,
                                "Solver: Jacobian assembly");

  // Set up local aliases
  using CellIterator =
    typename dealii::DoFHandler<dim>::active_cell_iterator;

  using CellFilter =
    dealii::FilteredIterator<
      typename dealii::DoFHandler<dim>::active_cell_iterator>;

  // Reset data
  jacobian = 0.0;

  // Set up the lambda function for the local assembly operation
  auto worker = [this](
    const CellIterator                         &cell,
    gCP::AssemblyData::Jacobian::Scratch<dim>  &scratch,
    gCP::AssemblyData::Jacobian::Copy          &data)
  {
    this->assemble_local_jacobian(cell, scratch, data);
  };

  // Set up the lambda function for the copy local to global operation
  auto copier = [this](const gCP::AssemblyData::Jacobian::Copy &data)
  {
    this->copy_local_to_global_jacobian(data);
  };

  // Define the update flags for the FEValues instances
  const dealii::UpdateFlags update_flags =
    dealii::update_JxW_values |
    dealii::update_values |
    dealii::update_gradients |
    dealii::update_quadrature_points;

  const dealii::UpdateFlags face_update_flags =
    dealii::update_JxW_values |
    dealii::update_normal_vectors |
    dealii::update_values |
    dealii::update_quadrature_points;

  // Assemble using the WorkStream approach
  dealii::WorkStream::run(
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               fe_field->get_dof_handler().begin_active()),
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               fe_field->get_dof_handler().end()),
    worker,
    copier,
    gCP::AssemblyData::Jacobian::Scratch<dim>(
      mapping_collection,
      quadrature_collection,
      face_quadrature_collection,
      fe_field->get_fe_collection(),
      update_flags,
      face_update_flags,
      crystals_data->get_n_slips()),
    gCP::AssemblyData::Jacobian::Copy(
      fe_field->get_fe_collection().max_dofs_per_cell()));

  // Compress global data
  jacobian.compress(dealii::VectorOperation::add);

  if (parameters.verbose)
    *pcout << " done!" << std::endl;
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::assemble_local_jacobian(
  const typename dealii::DoFHandler<dim>::active_cell_iterator  &cell,
  gCP::AssemblyData::Jacobian::Scratch<dim>                     &scratch,
  gCP::AssemblyData::Jacobian::Copy                             &data)
{
  // Reset local data
  data.local_matrix              = 0.0;
  data.cell_is_at_grain_boundary = false;

  data.neighbour_cells_local_dof_indices.clear();
  data.local_coupling_matrices.clear();

  // Local to global indices mapping
  cell->get_dof_indices(data.local_dof_indices);

  // Get the crystal identifier for the current cell
  const unsigned int crystal_id = cell->material_id();

  // Get the stiffness tetrad of the current crystal
  scratch.stiffness_tetrad =
    hooke_law->get_stiffness_tetrad(crystal_id);

  // Get the slips' symmetrized Schmid tensors of the current crystal
  scratch.symmetrized_schmid_tensors =
    crystals_data->get_symmetrized_schmid_tensors(crystal_id);

  // Update the hp::FEValues instance to the values of the current cell
  scratch.hp_fe_values.reinit(cell);

  const dealii::FEValues<dim> &fe_values =
    scratch.hp_fe_values.get_present_fe_values();

  // Get JxW values at the quadrature points
  scratch.JxW_values = fe_values.get_JxW_values();

  // Get values of the hardening variable at the quadrature points
  const std::vector<std::shared_ptr<QuadraturePointHistory<dim>>>
    local_quadrature_point_history =
      quadrature_point_history.get_data(cell);

  // Get values of the slips at the quadrature points
  for (unsigned int slip_id = 0;
      slip_id < crystals_data->get_n_slips();
      ++slip_id)
  {
    fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].get_function_values(
      trial_solution,
      scratch.slip_values[slip_id]);

    fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].get_function_values(
      fe_field->old_solution,
      scratch.old_slip_values[slip_id]);

    fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].get_function_gradients(
      trial_solution,
      scratch.slip_gradient_values[slip_id]);
  }

  // Loop over quadrature points
  for (unsigned int q_point = 0; q_point < scratch.n_q_points; ++q_point)
  {
    // Compute the jacobian of the scalar microscopic
    // stress w.r.t. slip at the current quadrature point
    scratch.scalar_microstress_law_jacobian_values[q_point] =
      scalar_microstress_law->get_jacobian(
        q_point,
        scratch.slip_values,
        scratch.old_slip_values,
        local_quadrature_point_history[q_point]->get_slip_resistances(),
        discrete_time.get_next_step_size());

    // Extract test function values at the quadrature points (Displacement)
    for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
    {
      scratch.sym_grad_vector_phi[i] =
        fe_values[fe_field->get_displacement_extractor(crystal_id)].symmetric_gradient(i,q_point);
    }

    // Extract test function values at the quadrature points (Slips)
    for (unsigned int slip_id = 0;
         slip_id < crystals_data->get_n_slips();
         ++slip_id)
    {
      scratch.vectorial_microstress_law_jacobian_values[q_point][slip_id] =
          vectorial_microstress_law->get_jacobian(
            crystal_id,
            slip_id,
            scratch.slip_gradient_values[slip_id][q_point]);

      for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
      {
        scratch.scalar_phi[slip_id][i] =
          fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].value(i,q_point);

        scratch.grad_scalar_phi[slip_id][i] =
          fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].gradient(i,q_point);
      }
    }

    // Loop over local degrees of freedom
    for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
    {
      for (unsigned int j = 0; j < scratch.dofs_per_cell; ++j)
      {
        if (fe_field->get_global_component(crystal_id, i) < dim)
        {
          if (fe_field->get_global_component(crystal_id, j) < dim)
          {
            data.local_matrix(i,j) +=
              scratch.sym_grad_vector_phi[i] *
              scratch.stiffness_tetrad *
              scratch.sym_grad_vector_phi[j] *
              scratch.JxW_values[q_point];

            AssertIsFinite(data.local_matrix(i,j));
          }
          else
          {
            const unsigned int slip_id_beta =
              fe_field->get_global_component(crystal_id, j) - dim;

            data.local_matrix(i,j) -=
              scratch.sym_grad_vector_phi[i] *
              scratch.stiffness_tetrad *
              scratch.symmetrized_schmid_tensors[slip_id_beta] *
              scratch.scalar_phi[slip_id_beta][j] *
              scratch.JxW_values[q_point];

            AssertIsFinite(data.local_matrix(i,j));
          }
        }
        else
        {
          const unsigned int slip_id_alpha =
              fe_field->get_global_component(crystal_id, i) - dim;

          if (fe_field->get_global_component(crystal_id, j) < dim)
          {
            data.local_matrix(i,j) -=
              scratch.scalar_phi[slip_id_alpha][i] *
              scratch.symmetrized_schmid_tensors[slip_id_alpha] *
              scratch.stiffness_tetrad *
              scratch.sym_grad_vector_phi[j] *
              scratch.JxW_values[q_point];

            AssertIsFinite(data.local_matrix(i,j));
          }
          else
          {
            const unsigned int slip_id_beta =
                fe_field->get_global_component(crystal_id, j) - dim;

            if (slip_id_alpha == slip_id_beta)
            {
              data.local_matrix(i,j) +=
                scratch.grad_scalar_phi[slip_id_alpha][i] *
                scratch.vectorial_microstress_law_jacobian_values[q_point][slip_id_alpha] *
                scratch.grad_scalar_phi[slip_id_beta][j] *
                scratch.JxW_values[q_point];
            }

            AssertIsFinite(data.local_matrix(i,j));

            data.local_matrix(i,j) -=
              scratch.scalar_phi[slip_id_alpha][i] *
              (-1.0 *
               scratch.symmetrized_schmid_tensors[slip_id_alpha] *
               scratch.stiffness_tetrad *
               scratch.symmetrized_schmid_tensors[slip_id_beta]
               -
               scratch.scalar_microstress_law_jacobian_values[q_point][slip_id_alpha][slip_id_beta]) *
              scratch.scalar_phi[slip_id_beta][j]*
              scratch.JxW_values[q_point];

            AssertIsFinite(data.local_matrix(i,j));
          }
        }
      }
    } // Loop over local degrees of freedom
  } // Loop over quadrature points

  // Grain boundary integral
  if (cell_is_at_grain_boundary(cell->active_cell_index()) &&
      (fe_field->is_decohesion_allowed() ||
       parameters.boundary_conditions_at_grain_boundaries ==
        RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction))
  {
    data.cell_is_at_grain_boundary = true;

    for (const auto &face_index : cell->face_indices())
      if (!cell->face(face_index)->at_boundary() &&
          cell->material_id() !=
            cell->neighbor(face_index)->material_id())
      {
        // Reset local data
        data.local_coupling_matrix = 0.0;

        // Local to global indices mapping of the neighbour cell
        cell->neighbor(face_index)->get_dof_indices(
          data.neighbour_cell_local_dof_indices);

        // Get the crystal identifier for the neighbour cell
        const unsigned int neighbour_crystal_id =
          cell->neighbor(face_index)->active_fe_index();

        // Update the hp::FEFaceValues instance to the values of the
        // current face
        scratch.hp_fe_face_values.reinit(cell, face_index);

        const dealii::FEFaceValues<dim> &fe_face_values =
          scratch.hp_fe_face_values.get_present_fe_values();

        // Update the hp::FEFaceValues instance to the values of the
        // neighbour face
        scratch.neighbour_hp_fe_face_values.reinit(
          cell->neighbor(face_index),
          cell->neighbor_of_neighbor(face_index));

        const dealii::FEFaceValues<dim> &neighbour_fe_face_values =
          scratch.neighbour_hp_fe_face_values.get_present_fe_values();

        // Get JxW values at the face quadrature points
        scratch.face_JxW_values = fe_face_values.get_JxW_values();

        // Get normal vector values values at the face quadrature points
        scratch.normal_vector_values = fe_face_values.get_normal_vectors();

        std::vector<std::shared_ptr<InterfaceQuadraturePointHistory<dim>>>
          local_interface_quadrature_point_history;

        // Get grain interactino moduli
        if (parameters.boundary_conditions_at_grain_boundaries ==
          RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction)
        {
          scratch.grain_interaction_moduli =
            microscopic_traction_law->get_grain_interaction_moduli(
              crystal_id,
              neighbour_crystal_id,
              scratch.normal_vector_values);
        }

        if (fe_field->is_decohesion_allowed())
        {
          // Get the internal variable values at the quadrature points
          local_interface_quadrature_point_history =
              interface_quadrature_point_history.get_data(
                cell->id(),
                cell->neighbor(face_index)->id());

          // Get JxW values at the quadrature points
          scratch.face_neighbor_JxW_values =
            neighbour_fe_face_values.get_JxW_values();

          fe_face_values[
            fe_field->get_displacement_extractor(crystal_id)].get_function_values(
            trial_solution,
            scratch.current_cell_displacement_values);

          fe_face_values[
            fe_field->get_displacement_extractor(crystal_id)].get_function_values(
            fe_field->old_solution,
            scratch.current_cell_old_displacement_values);

          neighbour_fe_face_values[
            fe_field->get_displacement_extractor(neighbour_crystal_id)].get_function_values(
            trial_solution,
            scratch.neighbor_cell_displacement_values);

          neighbour_fe_face_values[
            fe_field->get_displacement_extractor(neighbour_crystal_id)].get_function_values(
            fe_field->old_solution,
            scratch.neighbor_cell_old_displacement_values);
        }

        // Loop over face quadrature points
        for (unsigned int face_q_point = 0;
             face_q_point < scratch.n_face_q_points;
             ++face_q_point)
        {
          scratch.damage_variable_values[face_q_point] = 0.0;

          if (fe_field->is_decohesion_allowed())
          {
            scratch.damage_variable_values[face_q_point] =
              local_interface_quadrature_point_history[face_q_point]->
                get_damage_variable();

            const dealii::Tensor<1,dim> opening_displacement =
              scratch.neighbor_cell_displacement_values[face_q_point] -
              scratch.current_cell_displacement_values[face_q_point];

            scratch.cohesive_law_jacobian_values[face_q_point] =
              cohesive_law->get_jacobian(
                opening_displacement,
                scratch.normal_vector_values[face_q_point],
                local_interface_quadrature_point_history[face_q_point]->
                  get_max_effective_opening_displacement(),
                local_interface_quadrature_point_history[face_q_point]->
                      get_old_effective_opening_displacement(),
                discrete_time.get_next_step_size());

            scratch.contact_law_jacobian_values[face_q_point] =
              contact_law->get_jacobian(
                opening_displacement,
                scratch.normal_vector_values[face_q_point]);

            // Extract test function values at the quadrature points (Displacement)
            for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
            {
              scratch.face_vector_phi[i] =
                fe_face_values[fe_field->get_displacement_extractor(
                  crystal_id)].value(i, face_q_point);

              scratch.neighbor_face_vector_phi[i] =
                neighbour_fe_face_values[fe_field->get_displacement_extractor(
                  neighbour_crystal_id)].value(i, face_q_point);
            }
          }

          if (parameters.boundary_conditions_at_grain_boundaries ==
            RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction)
          {
            scratch.intra_gateaux_derivative_values[face_q_point] =
              microscopic_traction_law->get_intra_gateaux_derivative(
                face_q_point,
                scratch.grain_interaction_moduli);

            scratch.inter_gateaux_derivative_values[face_q_point] =
              microscopic_traction_law->get_inter_gateaux_derivative(
                face_q_point,
                scratch.grain_interaction_moduli);

            // Extract test function values at the quadrature points (Slips)
            for (unsigned int slip_id = 0;
                slip_id < crystals_data->get_n_slips();
                ++slip_id)
              for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
              {
                scratch.face_scalar_phi[slip_id][i] =
                  fe_face_values[fe_field->get_slip_extractor(
                    crystal_id, slip_id)].value(i,face_q_point);

                scratch.neighbour_face_scalar_phi[slip_id][i] =
                  neighbour_fe_face_values[fe_field->get_slip_extractor(
                    neighbour_crystal_id, slip_id)].value(i,face_q_point);
              }
          }

          // Loop over degrees of freedom
          for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
            for (unsigned int j = 0; j < scratch.dofs_per_cell; ++j)
              if (fe_field->get_global_component(crystal_id, i) < dim &&
                  fe_field->get_global_component(crystal_id, j) < dim)
              {
                if (fe_field->is_decohesion_allowed())
                {
                  data.local_matrix(i,j) -=
                    scratch.face_vector_phi[i] *
                    (cohesive_law->get_degradation_function_value(
                      scratch.damage_variable_values[face_q_point],
                      parameters.constitutive_laws_parameters.cohesive_law_parameters.flag_couple_macrotraction_to_damage) *
                     scratch.cohesive_law_jacobian_values[face_q_point]
                     +
                     scratch.contact_law_jacobian_values[face_q_point]) *
                    - 1.0 *
                    scratch.face_vector_phi[j] *
                    scratch.face_JxW_values[face_q_point];

                  data.local_coupling_matrix(i,j) -=
                    scratch.face_vector_phi[i] *
                    (cohesive_law->get_degradation_function_value(
                      scratch.damage_variable_values[face_q_point],
                      parameters.constitutive_laws_parameters.cohesive_law_parameters.flag_couple_macrotraction_to_damage) *
                     scratch.cohesive_law_jacobian_values[face_q_point]
                     +
                     scratch.contact_law_jacobian_values[face_q_point]) *
                    scratch.neighbor_face_vector_phi[j] *
                    scratch.face_JxW_values[face_q_point];

                  AssertIsFinite(data.local_matrix(i,j));
                  AssertIsFinite(data.local_coupling_matrix(i,j));
                }
              }
              else if (
                fe_field->get_global_component(crystal_id, i) >= dim &&
                fe_field->get_global_component(crystal_id, j) >= dim)
              {
                if (parameters.boundary_conditions_at_grain_boundaries ==
                    RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction)
                {
                  const unsigned int slip_id_alpha =
                    fe_field->get_global_component(crystal_id, i) - dim;

                  const unsigned int slip_id_beta =
                    fe_field->get_global_component(crystal_id, j) - dim;

                  const unsigned int neighbour_slip_id_beta =
                    fe_field->get_global_component(neighbour_crystal_id, j)
                    - dim;

                  data.local_matrix(i,j) -=
                    scratch.face_scalar_phi[slip_id_alpha][i] *
                    cohesive_law->get_degradation_function_value(
                      scratch.damage_variable_values[face_q_point],
                      parameters.constitutive_laws_parameters.cohesive_law_parameters.flag_couple_microtraction_to_damage) *
                    scratch.intra_gateaux_derivative_values[face_q_point][slip_id_alpha][slip_id_beta] *
                    scratch.face_scalar_phi[slip_id_beta][j] *
                    scratch.face_JxW_values[face_q_point];

                  data.local_coupling_matrix(i,j) -=
                    scratch.face_scalar_phi[slip_id_alpha][i] *
                    cohesive_law->get_degradation_function_value(
                      scratch.damage_variable_values[face_q_point],
                      parameters.constitutive_laws_parameters.cohesive_law_parameters.flag_couple_microtraction_to_damage) *
                    scratch.inter_gateaux_derivative_values[face_q_point][slip_id_alpha][neighbour_slip_id_beta] *
                    scratch.neighbour_face_scalar_phi[neighbour_slip_id_beta][j] *
                    scratch.face_JxW_values[face_q_point];

                  AssertIsFinite(data.local_matrix(i,j));
                  AssertIsFinite(data.local_coupling_matrix(i,j));
                }
              } // Loop over degrees of freedom
        } // Loop over face quadrature points

        data.neighbour_cells_local_dof_indices.emplace_back(
          data.neighbour_cell_local_dof_indices);
        data.local_coupling_matrices.emplace_back(
          data.local_coupling_matrix);
      } // Loop over cell's faces
  } // Grain boundary integral

}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::copy_local_to_global_jacobian(
  const gCP::AssemblyData::Jacobian::Copy &data)
{
  fe_field->get_newton_method_constraints().distribute_local_to_global(
    data.local_matrix,
    data.local_dof_indices,
    jacobian);

  if (data.cell_is_at_grain_boundary)
  {
    AssertThrow(
      data.local_coupling_matrices.size() ==
        data.neighbour_cells_local_dof_indices.size(),
      dealii::ExcDimensionMismatch(
        data.local_coupling_matrices.size(),
        data.neighbour_cells_local_dof_indices.size()));

    AssertThrow(
      data.local_coupling_matrices.size() > 0,
      dealii::ExcLowerRangeType<unsigned int>(
        data.local_coupling_matrices.size(), 0));

    for (unsigned int i = 0; i < data.local_coupling_matrices.size(); ++i)
      fe_field->get_newton_method_constraints().distribute_local_to_global(
        data.local_coupling_matrices[i],
        data.local_dof_indices,
        data.neighbour_cells_local_dof_indices[i],
        jacobian);
  }
}



template <int dim>
double GradientCrystalPlasticitySolver<dim>::assemble_residual()
{
  if (parameters.verbose)
    *pcout << std::setw(38) << std::left
           << "  Solver: Assembling residual...";

  dealii::TimerOutput::Scope  t(*timer_output,
                                "Solver: Residual assembly");

  // Set up local aliases
  using CellIterator =
    typename dealii::DoFHandler<dim>::active_cell_iterator;

  using CellFilter =
    dealii::FilteredIterator<
      typename dealii::DoFHandler<dim>::active_cell_iterator>;

  // Reset data
  residual = 0.0;

  // Set up the lambda function for the local assembly operation
  auto worker = [this](
    const CellIterator                         &cell,
    gCP::AssemblyData::Residual::Scratch<dim>  &scratch,
    gCP::AssemblyData::Residual::Copy          &data)
  {
    this->assemble_local_residual(cell, scratch, data);
  };

  // Set up the lambda function for the copy local to global operation
  auto copier = [this](const gCP::AssemblyData::Residual::Copy  &data)
  {
    this->copy_local_to_global_residual(data);
  };

  // Define the update flags for the FEValues instances
  const dealii::UpdateFlags update_flags  =
    dealii::update_JxW_values |
    dealii::update_values |
    dealii::update_gradients |
    dealii::update_quadrature_points;

  const dealii::UpdateFlags face_update_flags  =
    dealii::update_JxW_values |
    dealii::update_normal_vectors |
    dealii::update_values |
    dealii::update_quadrature_points;

  // Assemble using the WorkStream approach
  dealii::WorkStream::run(
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               fe_field->get_dof_handler().begin_active()),
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               fe_field->get_dof_handler().end()),
    worker,
    copier,
    gCP::AssemblyData::Residual::Scratch<dim>(
      mapping_collection,
      quadrature_collection,
      face_quadrature_collection,
      fe_field->get_fe_collection(),
      update_flags,
      face_update_flags,
      crystals_data->get_n_slips()),
    gCP::AssemblyData::Residual::Copy(
      fe_field->get_fe_collection().max_dofs_per_cell()));

  // Compress global data
  residual.compress(dealii::VectorOperation::add);

  residual_norm = residual.l2_norm();

  ghost_residual = residual;

  if (parameters.verbose)
    *pcout << " done!" << std::endl;

  return (0.5 * residual_norm * residual_norm);
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::assemble_local_residual(
  const typename dealii::DoFHandler<dim>::active_cell_iterator  &cell,
  gCP::AssemblyData::Residual::Scratch<dim>                     &scratch,
  gCP::AssemblyData::Residual::Copy                             &data)
{
  // Reset local data
  data.local_rhs                          = 0.0;
  data.local_matrix_for_inhomogeneous_bcs = 0.0;

  // Local to global mapping of the indices of the degrees of freedom
  cell->get_dof_indices(data.local_dof_indices);

  // Get the crystal identifier for the current cell
  const unsigned int crystal_id = cell->material_id();

  // Update the hp::FEValues instance to the values of the current cell
  scratch.hp_fe_values.reinit(cell);

  const dealii::FEValues<dim> &fe_values =
    scratch.hp_fe_values.get_present_fe_values();

  // Get JxW values at the quadrature points
  scratch.JxW_values = fe_values.get_JxW_values();

  // Get the hardening variable values at the quadrature points
  const std::vector<std::shared_ptr<QuadraturePointHistory<dim>>>
    local_quadrature_point_history =
      quadrature_point_history.get_data(cell);

  // Get the linear strain tensor values at the quadrature points
  fe_values[fe_field->get_displacement_extractor(crystal_id)].get_function_symmetric_gradients(
    trial_solution,
    scratch.strain_tensor_values);

  // Get the supply term values at the quadrature points
  if (supply_term.get() != nullptr)
    supply_term->value_list(
      fe_values.get_quadrature_points(),
      scratch.supply_term_values);

  // Get the slips and their gradients values at the quadrature points
  for (unsigned int slip_id = 0;
      slip_id < crystals_data->get_n_slips();
      ++slip_id)
  {
    fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].get_function_values(
      trial_solution,
      scratch.slip_values[slip_id]);

    fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].get_function_values(
      fe_field->old_solution,
      scratch.old_slip_values[slip_id]);

    fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].get_function_gradients(
      trial_solution,
      scratch.slip_gradient_values[slip_id]);
  }

  // Loop over quadrature points
  for (unsigned int q_point = 0; q_point < scratch.n_q_points; ++q_point)
  {
    // Compute the elastic strain tensor at the quadrature point
    scratch.elastic_strain_tensor_values[q_point] =
      macroscopic_strain +
      elastic_strain->get_elastic_strain_tensor(
        crystal_id,
        q_point,
        scratch.strain_tensor_values[q_point],
        scratch.slip_values);

    // Compute the stress tensor at the quadrature point
    scratch.stress_tensor_values[q_point] =
      hooke_law->get_stress_tensor(
        crystal_id,
        scratch.elastic_strain_tensor_values[q_point]);

    // Compute the resolved stress, scalar microscopic stress and
    // vector microscopic stress values at the quadrature point
    for (unsigned int slip_id = 0;
         slip_id < crystals_data->get_n_slips();
         ++slip_id)
    {
      scratch.vectorial_microstress_values[slip_id][q_point] =
        vectorial_microstress_law->get_vectorial_microstress(
          crystal_id,
          slip_id,
          scratch.slip_gradient_values[slip_id][q_point]);

      scratch.resolved_stress_values[slip_id][q_point] =
        resolved_shear_stress_law->get_resolved_shear_stress(
          crystal_id,
          slip_id,
          scratch.stress_tensor_values[q_point]);

      scratch.scalar_microstress_values[slip_id][q_point] =
        scalar_microstress_law->get_scalar_microstress(
          scratch.slip_values[slip_id][q_point],
          scratch.old_slip_values[slip_id][q_point],
          local_quadrature_point_history[q_point]->get_slip_resistance(slip_id),
          discrete_time.get_next_step_size());
    }

    // Extract test function values at the quadrature points (Displacements)
    for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
    {
      scratch.vector_phi[i] =
        fe_values[fe_field->get_displacement_extractor(crystal_id)].value(i,q_point);

      scratch.sym_grad_vector_phi[i] =
        fe_values[fe_field->get_displacement_extractor(crystal_id)].symmetric_gradient(i,q_point);
    }

    // Extract test function values at the quadrature points (Slips)
    for (unsigned int slip_id = 0;
         slip_id < crystals_data->get_n_slips();
         ++slip_id)
    {
      for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
      {
        scratch.scalar_phi[slip_id][i] =
          fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].value(i,q_point);

        scratch.grad_scalar_phi[slip_id][i] =
          fe_values[fe_field->get_slip_extractor(crystal_id, slip_id)].gradient(i,q_point);
      }
    }

    // Loop over the degrees of freedom
    for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
    {
      if (fe_field->get_global_component(crystal_id, i) < dim)
      {
        data.local_rhs(i) -=
          (scratch.sym_grad_vector_phi[i] *
           scratch.stress_tensor_values[q_point]
           -
           scratch.vector_phi[i] *
           scratch.supply_term_values[q_point]) *
          scratch.JxW_values[q_point];
      }
      else
      {
        const unsigned int slip_id =
                fe_field->get_global_component(crystal_id, i) - dim;

        data.local_rhs(i) -=
          (scratch.grad_scalar_phi[slip_id][i] *
           scratch.vectorial_microstress_values[slip_id][q_point]
           -
           scratch.scalar_phi[slip_id][i] *
           (scratch.resolved_stress_values[slip_id][q_point]
            -
            scratch.scalar_microstress_values[slip_id][q_point])) *
          scratch.JxW_values[q_point];
      }
    } // Loop over the degrees of freedom
  } // Loop over quadrature points

  // Grain boundary integral
  if (cell_is_at_grain_boundary(cell->active_cell_index()) &&
      (fe_field->is_decohesion_allowed() ||
       parameters.boundary_conditions_at_grain_boundaries ==
        RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction))
    for (const auto &face_index : cell->face_indices())
      if (!cell->face(face_index)->at_boundary() &&
          cell->material_id() !=
            cell->neighbor(face_index)->material_id())
      {
        // Get the crystal identifier for the neighbour cell
        const unsigned int neighbour_crystal_id =
          cell->neighbor(face_index)->active_fe_index();

        // Update the hp::FEFaceValues instance to the values of the
        // current face
        scratch.hp_fe_face_values.reinit(cell, face_index);

        const dealii::FEFaceValues<dim> &fe_face_values =
          scratch.hp_fe_face_values.get_present_fe_values();

        // Update the hp::FEFaceValues instance to the values of the
        // neighbour face
        scratch.neighbour_hp_fe_face_values.reinit(
          cell->neighbor(face_index),
          cell->neighbor_of_neighbor(face_index));

        const dealii::FEFaceValues<dim> &neighbour_fe_face_values =
          scratch.neighbour_hp_fe_face_values.get_present_fe_values();

        // Get JxW values at the quadrature points
        scratch.face_JxW_values = fe_face_values.get_JxW_values();

        // Get normal vector values values at the quadrature points
        scratch.normal_vector_values = fe_face_values.get_normal_vectors();

        if (parameters.boundary_conditions_at_grain_boundaries ==
          RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction)
        {
           // Get grain interactino moduli
          scratch.grain_interaction_moduli =
            microscopic_traction_law->get_grain_interaction_moduli(
              crystal_id,
              neighbour_crystal_id,
              scratch.normal_vector_values);

          // Get the values of the slips of the current and the neighbour
          // cell at the face quadrature points
          for (unsigned int slip_id = 0;
              slip_id < crystals_data->get_n_slips(); ++slip_id)
          {
            fe_face_values[fe_field->get_slip_extractor(
                crystal_id, slip_id)].get_function_values(
                  trial_solution,
                  scratch.face_slip_values[slip_id]);

            neighbour_fe_face_values[fe_field->get_slip_extractor(
                neighbour_crystal_id, slip_id)].get_function_values(
                  trial_solution,
                  scratch.neighbour_face_slip_values[slip_id]);
          }
        }

        std::vector<std::shared_ptr<InterfaceQuadraturePointHistory<dim>>>
          local_interface_quadrature_point_history;

        if (fe_field->is_decohesion_allowed())
        {
          // Get the internal variable values at the quadrature points
          local_interface_quadrature_point_history =
              interface_quadrature_point_history.get_data(
                cell->id(),
                cell->neighbor(face_index)->id());

          // Get JxW values at the quadrature points
          scratch.face_neighbor_JxW_values =
            neighbour_fe_face_values.get_JxW_values();

          fe_face_values[
            fe_field->get_displacement_extractor(crystal_id)].get_function_values(
            trial_solution,
            scratch.current_cell_displacement_values);

          fe_face_values[
            fe_field->get_displacement_extractor(crystal_id)].get_function_values(
            fe_field->old_solution,
            scratch.current_cell_old_displacement_values);

          neighbour_fe_face_values[
            fe_field->get_displacement_extractor(neighbour_crystal_id)].get_function_values(
            trial_solution,
            scratch.neighbor_cell_displacement_values);

          neighbour_fe_face_values[
            fe_field->get_displacement_extractor(neighbour_crystal_id)].get_function_values(
            fe_field->old_solution,
            scratch.neighbor_cell_old_displacement_values);
        }

        // Loop over face quadrature points
        for (unsigned int face_q_point = 0;
             face_q_point < scratch.n_face_q_points; ++face_q_point)
        {
          if (parameters.boundary_conditions_at_grain_boundaries ==
            RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction)
            for (unsigned int slip_id = 0;
                slip_id < crystals_data->get_n_slips(); ++slip_id)
            {
              // Compute the microscopic traction values at the
              // quadrature point
              scratch.microscopic_traction_values[slip_id][face_q_point] =
                microscopic_traction_law->get_microscopic_traction(
                  face_q_point,
                  slip_id,
                  scratch.grain_interaction_moduli,
                  scratch.face_slip_values,
                  scratch.neighbour_face_slip_values);

              // Extract test function values at the quadrature points (Slips)
              for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
                scratch.face_scalar_phi[slip_id][i] =
                  fe_face_values[fe_field->get_slip_extractor(
                    crystal_id, slip_id)].value(i,face_q_point);
            }

          scratch.damage_variable_values[face_q_point] = 0.0;

          if (fe_field->is_decohesion_allowed())
          {
            scratch.damage_variable_values[face_q_point] =
              local_interface_quadrature_point_history[face_q_point]->
                get_damage_variable();

            const dealii::Tensor<1,dim> opening_displacement =
              scratch.neighbor_cell_displacement_values[face_q_point] -
              scratch.current_cell_displacement_values[face_q_point];

            scratch.cohesive_traction_values[face_q_point] =
              cohesive_law->get_cohesive_traction(
                opening_displacement,
                scratch.normal_vector_values[face_q_point],
                local_interface_quadrature_point_history[face_q_point]->
                  get_max_effective_opening_displacement(),
                local_interface_quadrature_point_history[face_q_point]->
                  get_old_effective_opening_displacement(),
                discrete_time.get_next_step_size());

            scratch.contact_traction_values[face_q_point] =
              contact_law->get_contact_traction(
                opening_displacement,
                scratch.normal_vector_values[face_q_point]);

            // Extract test function values at the quadrature points (Slips)
            for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
              scratch.face_vector_phi[i] =
                fe_face_values[fe_field->get_displacement_extractor(
                  crystal_id)].value(i, face_q_point);
          }

          // Loop over degrees of freedom
          for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
            if (fe_field->get_global_component(crystal_id, i) < dim)
            {
              if (fe_field->is_decohesion_allowed())
                data.local_rhs(i) +=
                  scratch.face_vector_phi[i] *
                  (cohesive_law->get_degradation_function_value(
                    scratch.damage_variable_values[face_q_point],
                    parameters.constitutive_laws_parameters.cohesive_law_parameters.flag_couple_macrotraction_to_damage) *
                   scratch.cohesive_traction_values[face_q_point]
                   +
                   scratch.contact_traction_values[face_q_point])*
                  scratch.face_JxW_values[face_q_point];

              AssertIsFinite(data.local_rhs(i));
            }
            else
            {
              if (parameters.boundary_conditions_at_grain_boundaries ==
                  RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction)
              {
                const unsigned int slip_id =
                  fe_field->get_global_component(crystal_id, i) - dim;

                data.local_rhs(i) +=
                  scratch.face_scalar_phi[slip_id][i] *
                  cohesive_law->get_degradation_function_value(
                    scratch.damage_variable_values[face_q_point],
                    parameters.constitutive_laws_parameters.cohesive_law_parameters.flag_couple_microtraction_to_damage) *
                  scratch.microscopic_traction_values[slip_id][face_q_point] *
                  scratch.face_JxW_values[face_q_point];
              }

              AssertIsFinite(data.local_rhs(i));
            } // Loop over degrees of freedom
        } // Loop over face quadrature points
      } // Loop over cell's faces


  // Boundary integral
  if (!neumann_boundary_conditions.empty() && cell->at_boundary())
    for (const auto &face : cell->face_iterators())
      if (face->at_boundary() &&
          neumann_boundary_conditions.find(face->boundary_id()) !=
            neumann_boundary_conditions.end())
      {
        //std::cout << face->boundary_id() << std::endl << std::endl;

        // Update the hp::FEFaceValues instance to the values of the current cell
        scratch.hp_fe_face_values.reinit(cell, face);

        const dealii::FEFaceValues<dim> &fe_face_values =
          scratch.hp_fe_face_values.get_present_fe_values();

        // Compute the Neumann boundary function values at the
        // quadrature points
        neumann_boundary_conditions.at(face->boundary_id())->set_time(
          discrete_time.get_next_time());

        neumann_boundary_conditions.at(face->boundary_id())->value_list(
          fe_face_values.get_quadrature_points(),
          scratch.neumann_boundary_values);

        // Get JxW values at the quadrature points
        scratch.face_JxW_values = fe_face_values.get_JxW_values();

        // Loop over face quadrature points
        for (unsigned int face_q_point = 0;
             face_q_point < scratch.n_face_q_points; ++face_q_point)
        {
          // Extract the test function's values at the face quadrature points
          for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
            scratch.face_vector_phi[i] =
              fe_face_values[fe_field->get_displacement_extractor(crystal_id)].value(i,face_q_point);

          // Loop over degrees of freedom
          for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
            data.local_rhs(i) +=
              scratch.face_vector_phi[i] *
              scratch.neumann_boundary_values[face_q_point] *
              scratch.face_JxW_values[face_q_point];
        } // Loop over face quadrature points
      } // if (face->at_boundary() && face->boundary_id() == 3)

}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::copy_local_to_global_residual(
  const gCP::AssemblyData::Residual::Copy &data)
{
  fe_field->get_newton_method_constraints().distribute_local_to_global(
    data.local_rhs,
    data.local_dof_indices,
    residual,
    data.local_matrix_for_inhomogeneous_bcs);
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::prepare_quadrature_point_history()
{
  const unsigned int n_q_points =
    quadrature_collection.max_n_quadrature_points();

  const unsigned int n_face_q_points =
    face_quadrature_collection.max_n_quadrature_points();

  for (const auto &cell : fe_field->get_triangulation().active_cell_iterators())
    if (cell->is_locally_owned())
    {
      const std::vector<std::shared_ptr<QuadraturePointHistory<dim>>>
        local_quadrature_point_history =
          quadrature_point_history.get_data(cell);

      Assert(local_quadrature_point_history.size() == n_q_points,
              dealii::ExcInternalError());

      for (unsigned int q_point = 0;
            q_point < n_q_points;
            ++q_point)
        local_quadrature_point_history[q_point]->store_current_values();

      if (cell_is_at_grain_boundary(cell->active_cell_index()) &&
          fe_field->is_decohesion_allowed())
        for (const auto &face_index : cell->face_indices())
          if (!cell->face(face_index)->at_boundary() &&
              cell->material_id() !=
                cell->neighbor(face_index)->material_id())
          {
            const std::vector<std::shared_ptr<InterfaceQuadraturePointHistory<dim>>>
              local_interface_quadrature_point_history =
                interface_quadrature_point_history.get_data(
                  cell->id(),
                  cell->neighbor(face_index)->id());

            Assert(local_interface_quadrature_point_history.size() ==
                     n_face_q_points,
                   dealii::ExcInternalError());

            for (unsigned int face_q_point = 0;
                  face_q_point < n_face_q_points; ++face_q_point)
              local_interface_quadrature_point_history[face_q_point]->store_current_values();
          }
    }
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::reset_quadrature_point_history()
{
  const unsigned int n_quadrature_points =
    quadrature_collection.max_n_quadrature_points();

  const unsigned int n_face_quadrature_points =
    face_quadrature_collection.max_n_quadrature_points();

  for (const auto &active_cell :
       fe_field->get_triangulation().active_cell_iterators())
  {
    if (active_cell->is_locally_owned())
    {
      const std::vector<std::shared_ptr<QuadraturePointHistory<dim>>>
        local_quadrature_point_history =
          quadrature_point_history.get_data(active_cell);

      Assert(local_quadrature_point_history.size() ==
              n_quadrature_points,
             dealii::ExcInternalError());

      for (unsigned int quadrature_point = 0;
           quadrature_point < n_quadrature_points;
           ++quadrature_point)
      {
        local_quadrature_point_history[quadrature_point]->
          reset_values();
      }

      if (cell_is_at_grain_boundary(active_cell->active_cell_index()) &&
          fe_field->is_decohesion_allowed())
      {
        for (const auto &face_index : active_cell->face_indices())
        {
          if (!active_cell->face(face_index)->at_boundary() &&
              active_cell->material_id() !=
                active_cell->neighbor(face_index)->material_id())
          {
            const std::vector<std::shared_ptr<
              InterfaceQuadraturePointHistory<dim>>>
                local_interface_quadrature_point_history =
                  interface_quadrature_point_history.get_data(
                    active_cell->id(),
                    active_cell->neighbor(face_index)->id());

            Assert(local_interface_quadrature_point_history.size() ==
                     n_face_quadrature_points,
                   dealii::ExcInternalError());

            for (unsigned int face_quadrature_point = 0;
                 face_quadrature_point < n_face_quadrature_points;
                 ++face_quadrature_point)
            {
              local_interface_quadrature_point_history[face_quadrature_point]->
                reset_values();
            }
          }
        }
      }
    }
  }
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::reset_and_update_quadrature_point_history()
{
  dealii::TimerOutput::Scope  t(*timer_output,
                                "Solver: Reset and update quadrature point history");

  // Set up local aliases
  using CellIterator =
    typename dealii::DoFHandler<dim>::active_cell_iterator;

  using CellFilter =
    dealii::FilteredIterator<
      typename dealii::DoFHandler<dim>::active_cell_iterator>;

  // Set up the lambda function for the local assembly operation
  auto worker = [this](
    const CellIterator                                      &cell,
    gCP::AssemblyData::QuadraturePointHistory::Scratch<dim> &scratch,
    gCP::AssemblyData::QuadraturePointHistory::Copy         &data)
  {
    this->update_local_quadrature_point_history(cell, scratch, data);
  };

  // Set up the lambda function for the copy local to global operation
  auto copier = [this](const gCP::AssemblyData::QuadraturePointHistory::Copy  &data)
  {
    this->copy_local_to_global_quadrature_point_history(data);
  };

  // Define the update flags for the FEValues instances
  const dealii::UpdateFlags update_flags  =
    dealii::update_values;

  const dealii::UpdateFlags face_update_flags  =
    dealii::update_values |
    dealii::update_normal_vectors;

  // Assemble using the WorkStream approach
  dealii::WorkStream::run(
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               fe_field->get_dof_handler().begin_active()),
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               fe_field->get_dof_handler().end()),
    worker,
    copier,
    gCP::AssemblyData::QuadraturePointHistory::Scratch<dim>(
      mapping_collection,
      quadrature_collection,
      face_quadrature_collection,
      fe_field->get_fe_collection(),
      update_flags,
      face_update_flags,
      crystals_data->get_n_slips()),
    gCP::AssemblyData::QuadraturePointHistory::Copy());
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::
update_local_quadrature_point_history(
  const typename dealii::DoFHandler<dim>::active_cell_iterator  &cell,
  gCP::AssemblyData::QuadraturePointHistory::Scratch<dim>       &scratch,
  gCP::AssemblyData::QuadraturePointHistory::Copy               &)
{
  // Get the crystal identifier for the current cell
  const unsigned int crystal_id = cell->material_id();

  // Get the local quadrature point history instance
  const std::vector<std::shared_ptr<QuadraturePointHistory<dim>>>
    local_quadrature_point_history =
      quadrature_point_history.get_data(cell);

  Assert(local_quadrature_point_history.size() == scratch.n_q_points,
         dealii::ExcInternalError());

  // Update the hp::FEValues instance to the values of the current cell
  scratch.hp_fe_values.reinit(cell);

  const dealii::FEValues<dim> &fe_values =
    scratch.hp_fe_values.get_present_fe_values();

  // Reset local data
  scratch.reset();

  // Get the slip values at the quadrature points
  for (unsigned int slip_id = 0;
       slip_id < fe_field->get_n_slips();
       ++slip_id)
  {
    fe_values[
      fe_field->get_slip_extractor(crystal_id,
                                   slip_id)].get_function_values(
      trial_solution,
      scratch.slips_values[slip_id]);

    fe_values[
      fe_field->get_slip_extractor(crystal_id,
                                   slip_id)].get_function_values(
      fe_field->old_solution,
      scratch.old_slips_values[slip_id]);
  }

  // Loop over quadrature points
  for (const unsigned int q_point : fe_values.quadrature_point_indices())
  {
    local_quadrature_point_history[q_point]->update_values(
      q_point,
      scratch.slips_values,
      scratch.old_slips_values);
  } // Loop over quadrature points

  if (cell_is_at_grain_boundary(cell->active_cell_index()) &&
      fe_field->is_decohesion_allowed())
    for (const auto &face_index : cell->face_indices())
      if (!cell->face(face_index)->at_boundary() &&
          cell->material_id() !=
            cell->neighbor(face_index)->material_id())
      {
        // Get the crystal identifier for the neighbor cell
        const unsigned int neighbor_crystal_id =
          cell->neighbor(face_index)->material_id();

        // Get the local quadrature point history instance
        const std::vector<std::shared_ptr<InterfaceQuadraturePointHistory<dim>>>
          local_interface_quadrature_point_history =
            interface_quadrature_point_history.get_data(
              cell->id(),
              cell->neighbor(face_index)->id());

        Assert(local_interface_quadrature_point_history.size() ==
                 scratch.n_face_q_points,
               dealii::ExcInternalError());

        // Update the hp::FEFaceValues instance to the values of the
        // current face
        scratch.hp_fe_face_values.reinit(cell, face_index);

        const dealii::FEFaceValues<dim> &fe_face_values =
          scratch.hp_fe_face_values.get_present_fe_values();

        // Update the hp::FEFaceValues instance to the values of the
        // neighbor face
        scratch.neighbor_hp_fe_face_values.reinit(
          cell->neighbor(face_index),
          cell->neighbor_of_neighbor(face_index));

        const dealii::FEFaceValues<dim> &neighbor_fe_face_values =
          scratch.neighbor_hp_fe_face_values.get_present_fe_values();

        // Get the displacement values
        fe_face_values[
          fe_field->get_displacement_extractor(crystal_id)].get_function_values(
          trial_solution,
          scratch.current_cell_displacement_values);

        neighbor_fe_face_values[
          fe_field->get_displacement_extractor(neighbor_crystal_id)].get_function_values(
          trial_solution,
          scratch.neighbor_cell_displacement_values);

        // Get plastic slips
        for (unsigned int slip_id = 0;
             slip_id < crystals_data->get_n_slips(); ++slip_id)
        {
          fe_face_values[fe_field->get_slip_extractor(
              crystal_id, slip_id)].get_function_values(
                trial_solution,
                scratch.face_slip_values[slip_id]);

          neighbor_fe_face_values[fe_field->get_slip_extractor(
              neighbor_crystal_id, slip_id)].get_function_values(
                trial_solution,
                scratch.neighbor_face_slip_values[slip_id]);
        }

        // Get normal vector values values at the quadrature points
        scratch.normal_vector_values =
          fe_face_values.get_normal_vectors();

        for (unsigned int face_q_point = 0;
             face_q_point < scratch.n_face_q_points; ++face_q_point)
        {
          switch (temporal_discretization_parameters.loading_type)
          {
            case RunTimeParameters::LoadingType::Monotonic:
              {
                local_interface_quadrature_point_history[face_q_point]->update_values(
                  scratch.neighbor_cell_displacement_values[face_q_point],
                  scratch.current_cell_displacement_values[face_q_point]);
              }
              break;

            case RunTimeParameters::LoadingType::Cyclic:
              {
                scratch.effective_opening_displacement[face_q_point] =
                  cohesive_law->get_effective_opening_displacement(
                    scratch.neighbor_cell_displacement_values[face_q_point] -
                    scratch.current_cell_displacement_values[face_q_point],
                    scratch.normal_vector_values[face_q_point]);

                scratch.thermodynamic_force_values[face_q_point] =
                  - cohesive_law->get_degradation_function_derivative_value(
                      local_interface_quadrature_point_history[face_q_point]->get_damage_variable(), true) *
                  (cohesive_law->get_free_energy_density(
                    scratch.effective_opening_displacement[face_q_point])
                   +
                   microscopic_traction_law->get_free_energy_density(
                    neighbor_crystal_id,
                    crystal_id,
                    face_q_point,
                    scratch.normal_vector_values,
                    scratch.neighbor_face_slip_values,
                    scratch.face_slip_values));

                const bool flag_currently_in_the_preloading_phase =
                  discrete_time.get_next_time() <=
                    temporal_discretization_parameters.start_of_loading_phase;

                const bool flag_no_damage_evolution =
                  parameters.flag_zero_damage_during_loading_and_unloading &&
                    flag_currently_in_the_preloading_phase;

                if (!flag_no_damage_evolution)
                {
                  local_interface_quadrature_point_history[face_q_point]->update_values(
                    scratch.effective_opening_displacement[face_q_point],
                    scratch.thermodynamic_force_values[face_q_point]);
                }
                else
                {
                  local_interface_quadrature_point_history[face_q_point]->update_values(
                    scratch.effective_opening_displacement[face_q_point]);
                }
              }
              break;

            case RunTimeParameters::LoadingType::CyclicWithUnloading:
              {
                scratch.effective_opening_displacement[face_q_point] =
                  cohesive_law->get_effective_opening_displacement(
                    scratch.neighbor_cell_displacement_values[face_q_point] -
                    scratch.current_cell_displacement_values[face_q_point],
                    scratch.normal_vector_values[face_q_point]);

                scratch.thermodynamic_force_values[face_q_point] =
                  - cohesive_law->get_degradation_function_derivative_value(
                      local_interface_quadrature_point_history[face_q_point]->get_damage_variable(), true) *
                  (cohesive_law->get_free_energy_density(
                    scratch.effective_opening_displacement[face_q_point])
                   +
                   microscopic_traction_law->get_free_energy_density(
                    neighbor_crystal_id,
                    crystal_id,
                    face_q_point,
                    scratch.normal_vector_values,
                    scratch.neighbor_face_slip_values,
                    scratch.face_slip_values));

                const bool flag_currently_in_the_preloading_phase =
                  discrete_time.get_next_time() <=
                    temporal_discretization_parameters.start_of_loading_phase;

                const bool flag_currently_in_the_unloading_phase =
                  discrete_time.get_next_time() >
                    temporal_discretization_parameters.start_of_unloading_phase;

                const bool contidion_A =
                  parameters.flag_zero_damage_during_loading_and_unloading &&
                  flag_currently_in_the_preloading_phase;

                const bool condition_B =
                  parameters.flag_zero_damage_during_loading_and_unloading &&
                  flag_currently_in_the_unloading_phase;

                if (contidion_A || condition_B)
                {
                  local_interface_quadrature_point_history[face_q_point]->update_values(
                    scratch.effective_opening_displacement[face_q_point]);
                }
                else
                {
                  local_interface_quadrature_point_history[face_q_point]->update_values(
                    scratch.effective_opening_displacement[face_q_point],
                    scratch.thermodynamic_force_values[face_q_point]);
                }
              }
              break;

            default:
              Assert(false, dealii::ExcNotImplemented());
              break;
          }
        }
      }
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::
store_effective_opening_displacement_in_quadrature_history()
{
  dealii::TimerOutput::Scope
    t(*timer_output, "Solver: Store effective opening displacement");

  // Set up local aliases
  using CellIterator =
    typename dealii::DoFHandler<dim>::active_cell_iterator;

  using CellFilter =
    dealii::FilteredIterator<
      typename dealii::DoFHandler<dim>::active_cell_iterator>;

  // Set up the lambda function for the local assembly operation
  auto worker = [this](
    const CellIterator                                      &cell,
    gCP::AssemblyData::QuadraturePointHistory::Scratch<dim> &scratch,
    gCP::AssemblyData::QuadraturePointHistory::Copy         &data)
  {
    this->store_local_effective_opening_displacement(cell, scratch, data);
  };

  // Set up the lambda function for the copy local to global operation
  auto copier = [this](const gCP::AssemblyData::QuadraturePointHistory::Copy  &data)
  {
    this->copy_local_to_global_quadrature_point_history(data);
  };

  // Define the update flags for the FEValues instances
  const dealii::UpdateFlags update_flags  =
    dealii::update_default;

  const dealii::UpdateFlags face_update_flags  =
    dealii::update_values |
    dealii::update_normal_vectors;

  // Assemble using the WorkStream approach
  dealii::WorkStream::run(
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               fe_field->get_dof_handler().begin_active()),
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               fe_field->get_dof_handler().end()),
    worker,
    copier,
    gCP::AssemblyData::QuadraturePointHistory::Scratch<dim>(
      mapping_collection,
      quadrature_collection,
      face_quadrature_collection,
      fe_field->get_fe_collection(),
      update_flags,
      face_update_flags,
      crystals_data->get_n_slips()),
    gCP::AssemblyData::QuadraturePointHistory::Copy());
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::
store_local_effective_opening_displacement(
  const typename dealii::DoFHandler<dim>::active_cell_iterator  &cell,
  gCP::AssemblyData::QuadraturePointHistory::Scratch<dim>       &scratch,
  gCP::AssemblyData::QuadraturePointHistory::Copy               &)
{
  // Get the crystal identifier for the current cell
  const unsigned int crystal_id = cell->material_id();

  // Reset local data
  scratch.reset();

  if (cell_is_at_grain_boundary(cell->active_cell_index()) &&
      fe_field->is_decohesion_allowed())
    for (const auto &face_index : cell->face_indices())
      if (!cell->face(face_index)->at_boundary() &&
          cell->material_id() !=
            cell->neighbor(face_index)->material_id())
      {
        // Get the crystal identifier for the neighbor cell
        const unsigned int neighbor_crystal_id =
          cell->neighbor(face_index)->material_id();

        // Get the local quadrature point history instance
        const std::vector<std::shared_ptr<InterfaceQuadraturePointHistory<dim>>>
          local_interface_quadrature_point_history =
            interface_quadrature_point_history.get_data(
              cell->id(),
              cell->neighbor(face_index)->id());

        Assert(local_interface_quadrature_point_history.size() ==
                 scratch.n_face_q_points,
               dealii::ExcInternalError());

        // Update the hp::FEFaceValues instance to the values of the
        // current face
        scratch.hp_fe_face_values.reinit(cell, face_index);

        const dealii::FEFaceValues<dim> &fe_face_values =
          scratch.hp_fe_face_values.get_present_fe_values();

        // Update the hp::FEFaceValues instance to the values of the
        // neighbor face
        scratch.neighbor_hp_fe_face_values.reinit(
          cell->neighbor(face_index),
          cell->neighbor_of_neighbor(face_index));

        const dealii::FEFaceValues<dim> &neighbor_fe_face_values =
          scratch.neighbor_hp_fe_face_values.get_present_fe_values();

        // Get the displacement values
        fe_face_values[
          fe_field->get_displacement_extractor(crystal_id)].get_function_values(
          trial_solution,
          scratch.current_cell_displacement_values);

        fe_face_values[
          fe_field->get_displacement_extractor(crystal_id)].get_function_values(
          fe_field->old_solution,
          scratch.current_cell_old_displacement_values);

        neighbor_fe_face_values[
          fe_field->get_displacement_extractor(neighbor_crystal_id)].get_function_values(
          trial_solution,
          scratch.neighbor_cell_displacement_values);

        neighbor_fe_face_values[
          fe_field->get_displacement_extractor(neighbor_crystal_id)].get_function_values(
          fe_field->old_solution,
          scratch.neighbor_cell_old_displacement_values);

        // Get normal vector values values at the quadrature points
        scratch.normal_vector_values =
          fe_face_values.get_normal_vectors();

        for (unsigned int face_q_point = 0;
             face_q_point < scratch.n_face_q_points; ++face_q_point)
        {
          scratch.cohesive_traction_values[face_q_point] =
            cohesive_law->get_cohesive_traction(
              scratch.neighbor_cell_displacement_values[face_q_point] -
              scratch.current_cell_displacement_values[face_q_point],
              scratch.normal_vector_values[face_q_point],
              local_interface_quadrature_point_history[face_q_point]->
                get_max_effective_opening_displacement(),
              (scratch.neighbor_cell_old_displacement_values[face_q_point] -
               scratch.current_cell_old_displacement_values[face_q_point]).norm(),
              discrete_time.get_next_step_size());

          local_interface_quadrature_point_history[face_q_point]->store_effective_opening_displacement(
            scratch.neighbor_cell_displacement_values[face_q_point],
            scratch.current_cell_displacement_values[face_q_point],
            scratch.normal_vector_values[face_q_point],
            (parameters.constitutive_laws_parameters.cohesive_law_parameters.flag_couple_macrotraction_to_damage ?
              std::pow(1.0 - local_interface_quadrature_point_history[face_q_point]->get_damage_variable(),
                        parameters.constitutive_laws_parameters.cohesive_law_parameters.degradation_exponent) :
              1.0 ) *
            scratch.cohesive_traction_values[face_q_point].norm());

          if (false/*print_out*/)
          {
            table_handler.add_value(
              "effective_opening_displacement",
              local_interface_quadrature_point_history[face_q_point]->
                get_effective_opening_displacement());
            table_handler.add_value("effective_traction_vector",
              local_interface_quadrature_point_history[face_q_point]->
                get_effective_cohesive_traction());
            table_handler.add_value("time",
              discrete_time.get_next_time());
            table_handler.add_value("damage_variable",
              local_interface_quadrature_point_history[face_q_point]->
                get_damage_variable());

            print_out = false;
          }
        }
      }
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::assemble_projection_matrix()
{
  // Set up local aliases
  using CellIterator =
    typename dealii::DoFHandler<dim>::active_cell_iterator;

  using CellFilter =
    dealii::FilteredIterator<
      typename dealii::DoFHandler<dim>::active_cell_iterator>;

  // Reset data
  lumped_projection_matrix = 0.0;

  // Set up the lambda function for the local assembly operation
  auto worker = [this](
    const CellIterator                                                &cell,
    gCP::AssemblyData::Postprocessing::ProjectionMatrix::Scratch<dim> &scratch,
    gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy         &data)
  {
    this->assemble_local_projection_matrix(cell, scratch, data);
  };

  // Set up the lambda function for the copy local to global operation
  auto copier = [this](
    const gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy &data)
  {
    this->copy_local_to_global_projection_matrix(data);
  };

  // Define the update flags for the FEValues instances
  const dealii::UpdateFlags update_flags =
    dealii::update_JxW_values |
    dealii::update_values;

  // Assemble using the WorkStream approach
  dealii::WorkStream::run(
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               projection_dof_handler.begin_active()),
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               projection_dof_handler.end()),
    worker,
    copier,
    gCP::AssemblyData::Postprocessing::ProjectionMatrix::Scratch<dim>(
      mapping_collection,
      quadrature_collection,
      face_quadrature_collection,
      projection_fe_collection,
      update_flags),
    gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy(
      projection_fe_collection.max_dofs_per_cell()));

  // Compress global data
  lumped_projection_matrix.compress(dealii::VectorOperation::add);
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::assemble_local_projection_matrix(
  const typename dealii::DoFHandler<dim>::active_cell_iterator      &cell,
  gCP::AssemblyData::Postprocessing::ProjectionMatrix::Scratch<dim> &scratch,
  gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy         &data)
{
  // Reset local data
  data.local_lumped_projection_matrix     = 0.0;
  data.local_matrix_for_inhomogeneous_bcs = 0.0;
  data.cell_is_at_grain_boundary          = false;

  // Grain boundary integral
  if (cell_is_at_grain_boundary(cell->active_cell_index()) &&
      (fe_field->is_decohesion_allowed() ||
       parameters.boundary_conditions_at_grain_boundaries ==
        RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction))
  {
    // Indicate the Copy struct that its a cell at the grain boundary
    data.cell_is_at_grain_boundary = true;

    // Local to global indices mapping
    cell->get_dof_indices(data.local_dof_indices);

    // Scalar extractor for the damage variable
    const dealii::FEValuesExtractors::Scalar  extractor(0);

    for (const auto &face_index : cell->face_indices())
      if (!cell->face(face_index)->at_boundary() &&
          cell->material_id() !=
            cell->neighbor(face_index)->material_id())
      {
        // Update the hp::FEFaceValues instance to the values of the
        // current face
        scratch.hp_fe_face_values.reinit(cell, face_index);

        const dealii::FEFaceValues<dim> &fe_face_values =
          scratch.hp_fe_face_values.get_present_fe_values();

        // Get JxW values at the face quadrature points
        scratch.face_JxW_values = fe_face_values.get_JxW_values();

        // Loop over quadrature points
        for (unsigned int face_q_point = 0;
             face_q_point < scratch.n_face_q_points; ++face_q_point)
        {
          // Extract test function values at the quadrature points (Displacement)
          for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
          {
            scratch.scalar_test_function[i] =
              fe_face_values[extractor].value(i, face_q_point);
          }

          // Loop over local degrees of freedom
          for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
            for (unsigned int j = 0; j < scratch.dofs_per_cell; ++j)
                  data.local_lumped_projection_matrix(i) +=
                    scratch.scalar_test_function[i] *
                    scratch.scalar_test_function[j] *
                    scratch.face_JxW_values[face_q_point];
        } // Loop over quadrature points
      }
  }
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::copy_local_to_global_projection_matrix(
  const gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy &data)
{
  if (data.cell_is_at_grain_boundary)
    projection_hanging_node_constraints.distribute_local_to_global(
      data.local_lumped_projection_matrix,
      data.local_dof_indices,
      lumped_projection_matrix,
      data.local_matrix_for_inhomogeneous_bcs);
}



template <int dim>
void GradientCrystalPlasticitySolver<dim>::assemble_projection_rhs()
{
  // Set up local aliases
  using CellIterator =
    typename dealii::DoFHandler<dim>::active_cell_iterator;

  using CellFilter =
    dealii::FilteredIterator<
      typename dealii::DoFHandler<dim>::active_cell_iterator>;

  // Reset data
  projection_rhs = 0.0;

  // Set up the lambda function for the local assembly operation
  auto worker = [this](
    const CellIterator                                             &cell,
    gCP::AssemblyData::Postprocessing::ProjectionRHS::Scratch<dim> &scratch,
    gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy         &data)
  {
    this->assemble_local_projection_rhs(cell, scratch, data);
  };

  // Set up the lambda function for the copy local to global operation
  auto copier = [this](
    const gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy &data)
  {
    this->copy_local_to_global_projection_rhs(data);
  };

  // Define the update flags for the FEValues instances
  const dealii::UpdateFlags update_flags =
    dealii::update_values |
    dealii::update_JxW_values;

  // Assemble using the WorkStream approach
  dealii::WorkStream::run(
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               projection_dof_handler.begin_active()),
    CellFilter(dealii::IteratorFilters::LocallyOwnedCell(),
               projection_dof_handler.end()),
    worker,
    copier,
    gCP::AssemblyData::Postprocessing::ProjectionRHS::Scratch<dim>(
      mapping_collection,
      quadrature_collection,
      face_quadrature_collection,
      projection_fe_collection,
      update_flags),
    gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy(
      projection_fe_collection.max_dofs_per_cell()));

  // Compress global data
  projection_rhs.compress(dealii::VectorOperation::add);
}


template <int dim>
void GradientCrystalPlasticitySolver<dim>::assemble_local_projection_rhs(
  const typename dealii::DoFHandler<dim>::active_cell_iterator    &cell,
  gCP::AssemblyData::Postprocessing::ProjectionRHS::Scratch<dim>  &scratch,
  gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy          &data)
{
  // Reset local data
  data.local_rhs                          = 0.0;
  data.local_matrix_for_inhomogeneous_bcs = 0.0;
  data.cell_is_at_grain_boundary          = false;

  // Grain boundary integral
  if (cell_is_at_grain_boundary(cell->active_cell_index()) &&
      (fe_field->is_decohesion_allowed() ||
       parameters.boundary_conditions_at_grain_boundaries ==
        RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction))
  {
    // Indicate the Copy struct that its a cell at the grain boundary
    data.cell_is_at_grain_boundary = true;

    // Local to global indices mapping
    cell->get_dof_indices(data.local_dof_indices);

    // Scalar extractor for the damage variable
    const dealii::FEValuesExtractors::Scalar  extractor(0);

    // Instance of the interface quadrature point history
    std::vector<std::shared_ptr<InterfaceQuadraturePointHistory<dim>>>
      local_interface_quadrature_point_history;

    for (const auto &face_index : cell->face_indices())
      if (!cell->face(face_index)->at_boundary() &&
          cell->material_id() !=
            cell->neighbor(face_index)->material_id())
      {
        // Update the hp::FEFaceValues instance to the values of the
        // current face
        scratch.hp_fe_face_values.reinit(cell, face_index);

        const dealii::FEFaceValues<dim> &fe_face_values =
          scratch.hp_fe_face_values.get_present_fe_values();

        // Get JxW values at the face quadrature points
        scratch.face_JxW_values = fe_face_values.get_JxW_values();

        // Get the internal variable values at the quadrature points
        local_interface_quadrature_point_history =
            interface_quadrature_point_history.get_data(
              cell->id(),
              cell->neighbor(face_index)->id());

        // Loop over quadrature points
        for (unsigned int face_q_point = 0;
             face_q_point < scratch.n_face_q_points; ++face_q_point)
        {
          scratch.damage_variable_values[face_q_point] = 0.0;

          scratch.damage_variable_values[face_q_point] =
            local_interface_quadrature_point_history[face_q_point]->
              get_damage_variable();

          // Extract test function values at the quadrature points (Displacement)
          for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
          {
            scratch.scalar_test_function[i] =
              fe_face_values[extractor].value(i, face_q_point);
          }

          // Loop over local degrees of freedom
          for (unsigned int i = 0; i < scratch.dofs_per_cell; ++i)
            data.local_rhs(i) +=
              scratch.scalar_test_function[i] *
              scratch.damage_variable_values[face_q_point] *
              scratch.face_JxW_values[face_q_point];
        } // Loop over quadrature points
      }
  }
}

template <int dim>
void GradientCrystalPlasticitySolver<dim>::copy_local_to_global_projection_rhs(
  const gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy &data)
{
  if (data.cell_is_at_grain_boundary)
    projection_hanging_node_constraints.distribute_local_to_global(
      data.local_rhs,
      data.local_dof_indices,
      projection_rhs,
      data.local_matrix_for_inhomogeneous_bcs);
}



template <int dim>
double GradientCrystalPlasticitySolver<dim>::get_macroscopic_damage()
{
  // Initiate the local integral value and at each wall.
  double macroscopic_damage_variable = 0.0;

  const dealii::UpdateFlags update_flags =
    dealii::update_JxW_values;

  // Finite element values
  dealii::hp::FEFaceValues<dim> hp_fe_face_values(
    mapping_collection,
    projection_fe_collection,
    face_quadrature_collection,
    update_flags);

  // Scalar extractor for the damage variable
  const dealii::FEValuesExtractors::Scalar extractor(0);

  // Number of quadrature points
  const unsigned int n_face_quadrature_points =
    face_quadrature_collection.max_n_quadrature_points();

  // Vectors to stores the temperature gradients and normal vectors
  // at the quadrature points
  std::vector<double> JxW_values(n_face_quadrature_points);

  double              domain_integral_damage_variable;

  double              cell_integral_damage_variable;

  double              domain_volume = 0.;

  double              cell_volume = 0.;

  // Instance of the interface quadrature point history
  std::vector<std::shared_ptr<InterfaceQuadraturePointHistory<dim>>>
    local_interface_quadrature_point_history;

  for (const auto &cell : projection_dof_handler.active_cell_iterators())
    if (cell->is_locally_owned() &&
        cell_is_at_grain_boundary(cell->active_cell_index()) &&
        (fe_field->is_decohesion_allowed() ||
          parameters.boundary_conditions_at_grain_boundaries ==
          RunTimeParameters::BoundaryConditionsAtGrainBoundaries::Microtraction))
      for (const auto &face_index : cell->face_indices())
        if (!cell->face(face_index)->at_boundary() &&
            cell->material_id() !=
              cell->neighbor(face_index)->material_id())
        {
          // Reset local values
          cell_integral_damage_variable = 0.0;

          cell_volume                   = 0.0;

          // Update the hp::FEFaceValues instance to the values of the current cell
          hp_fe_face_values.reinit(cell, face_index);

          const dealii::FEFaceValues<dim> &fe_face_values =
            hp_fe_face_values.get_present_fe_values();

          // Get JxW values at the quadrature points
          JxW_values = fe_face_values.get_JxW_values();

          // Get the internal variable values at the quadrature points
          local_interface_quadrature_point_history =
              interface_quadrature_point_history.get_data(
                cell->id(),
                cell->neighbor(face_index)->id());

          // Numerical integration
          for (unsigned int quadrature_point_id = 0;
                quadrature_point_id < n_face_quadrature_points;
                ++quadrature_point_id)
          {
            cell_integral_damage_variable +=
              local_interface_quadrature_point_history[quadrature_point_id]->
                get_damage_variable() *
                JxW_values[quadrature_point_id];

            cell_volume += JxW_values[quadrature_point_id];
          }

          domain_integral_damage_variable +=
            cell_integral_damage_variable;

          domain_volume += cell_volume;
        }

  // Gather the values of each processor
  domain_integral_damage_variable =
    dealii::Utilities::MPI::sum(domain_integral_damage_variable,
                                MPI_COMM_WORLD);

  domain_volume =
    dealii::Utilities::MPI::sum(domain_volume, MPI_COMM_WORLD);

  // Compute the homogenized values
  macroscopic_damage_variable =
    domain_integral_damage_variable / domain_volume;

  return macroscopic_damage_variable;
}



} // namespace gCP



template void gCP::GradientCrystalPlasticitySolver<2>::assemble_jacobian();
template void gCP::GradientCrystalPlasticitySolver<3>::assemble_jacobian();

template void gCP::GradientCrystalPlasticitySolver<2>::assemble_local_jacobian(
  const typename dealii::DoFHandler<2>::active_cell_iterator  &,
  gCP::AssemblyData::Jacobian::Scratch<2>                     &,
  gCP::AssemblyData::Jacobian::Copy                           &);
template void gCP::GradientCrystalPlasticitySolver<3>::assemble_local_jacobian(
  const typename dealii::DoFHandler<3>::active_cell_iterator  &,
  gCP::AssemblyData::Jacobian::Scratch<3>                     &,
  gCP::AssemblyData::Jacobian::Copy                           &);

template void gCP::GradientCrystalPlasticitySolver<2>::copy_local_to_global_jacobian(
  const gCP::AssemblyData::Jacobian::Copy &);
template void gCP::GradientCrystalPlasticitySolver<3>::copy_local_to_global_jacobian(
  const gCP::AssemblyData::Jacobian::Copy &);

template double gCP::GradientCrystalPlasticitySolver<2>::assemble_residual();
template double gCP::GradientCrystalPlasticitySolver<3>::assemble_residual();

template void gCP::GradientCrystalPlasticitySolver<2>::assemble_local_residual(
  const typename dealii::DoFHandler<2>::active_cell_iterator  &,
  gCP::AssemblyData::Residual::Scratch<2>                     &,
  gCP::AssemblyData::Residual::Copy                           &);
template void gCP::GradientCrystalPlasticitySolver<3>::assemble_local_residual(
  const typename dealii::DoFHandler<3>::active_cell_iterator  &,
  gCP::AssemblyData::Residual::Scratch<3>                     &,
  gCP::AssemblyData::Residual::Copy                           &);

template void gCP::GradientCrystalPlasticitySolver<2>::copy_local_to_global_residual(
  const gCP::AssemblyData::Residual::Copy &);
template void gCP::GradientCrystalPlasticitySolver<3>::copy_local_to_global_residual(
  const gCP::AssemblyData::Residual::Copy &);

template void gCP::GradientCrystalPlasticitySolver<2>::prepare_quadrature_point_history();
template void gCP::GradientCrystalPlasticitySolver<3>::prepare_quadrature_point_history();

template void gCP::GradientCrystalPlasticitySolver<2>::reset_quadrature_point_history();
template void gCP::GradientCrystalPlasticitySolver<3>::reset_quadrature_point_history();

template void gCP::GradientCrystalPlasticitySolver<2>::reset_and_update_quadrature_point_history();
template void gCP::GradientCrystalPlasticitySolver<3>::reset_and_update_quadrature_point_history();

template void gCP::GradientCrystalPlasticitySolver<2>::update_local_quadrature_point_history(
  const typename dealii::DoFHandler<2>::active_cell_iterator  &,
  gCP::AssemblyData::QuadraturePointHistory::Scratch<2>       &,
  gCP::AssemblyData::QuadraturePointHistory::Copy             &);
template void gCP::GradientCrystalPlasticitySolver<3>::update_local_quadrature_point_history(
  const typename dealii::DoFHandler<3>::active_cell_iterator  &,
  gCP::AssemblyData::QuadraturePointHistory::Scratch<3>       &,
  gCP::AssemblyData::QuadraturePointHistory::Copy             &);

template void gCP::GradientCrystalPlasticitySolver<2>::
store_effective_opening_displacement_in_quadrature_history();
template void gCP::GradientCrystalPlasticitySolver<3>::
store_effective_opening_displacement_in_quadrature_history();

template void gCP::GradientCrystalPlasticitySolver<2>::
store_local_effective_opening_displacement(
  const typename dealii::DoFHandler<2>::active_cell_iterator  &,
  gCP::AssemblyData::QuadraturePointHistory::Scratch<2>       &,
  gCP::AssemblyData::QuadraturePointHistory::Copy             &);
template void gCP::GradientCrystalPlasticitySolver<3>::
store_local_effective_opening_displacement(
  const typename dealii::DoFHandler<3>::active_cell_iterator  &,
  gCP::AssemblyData::QuadraturePointHistory::Scratch<3>       &,
  gCP::AssemblyData::QuadraturePointHistory::Copy             &);

template void gCP::GradientCrystalPlasticitySolver<2>::assemble_projection_matrix();
template void gCP::GradientCrystalPlasticitySolver<3>::assemble_projection_matrix();

template void gCP::GradientCrystalPlasticitySolver<2>::assemble_local_projection_matrix(
  const typename dealii::DoFHandler<2>::active_cell_iterator      &,
  gCP::AssemblyData::Postprocessing::ProjectionMatrix::Scratch<2> &,
  gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy       &);
template void gCP::GradientCrystalPlasticitySolver<3>::assemble_local_projection_matrix(
  const typename dealii::DoFHandler<3>::active_cell_iterator      &,
  gCP::AssemblyData::Postprocessing::ProjectionMatrix::Scratch<3> &,
  gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy       &);

template void gCP::GradientCrystalPlasticitySolver<2>::copy_local_to_global_projection_matrix(
  const gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy &);
template void gCP::GradientCrystalPlasticitySolver<3>::copy_local_to_global_projection_matrix(
  const gCP::AssemblyData::Postprocessing::ProjectionMatrix::Copy &);

template void gCP::GradientCrystalPlasticitySolver<2>::assemble_projection_rhs();
template void gCP::GradientCrystalPlasticitySolver<3>::assemble_projection_rhs();

template void gCP::GradientCrystalPlasticitySolver<2>::assemble_local_projection_rhs(
  const typename dealii::DoFHandler<2>::active_cell_iterator    &,
  gCP::AssemblyData::Postprocessing::ProjectionRHS::Scratch<2>  &,
  gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy        &);
template void gCP::GradientCrystalPlasticitySolver<3>::assemble_local_projection_rhs(
  const typename dealii::DoFHandler<3>::active_cell_iterator    &,
  gCP::AssemblyData::Postprocessing::ProjectionRHS::Scratch<3>  &,
  gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy        &);

template void gCP::GradientCrystalPlasticitySolver<2>::copy_local_to_global_projection_rhs(
  const gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy &);
template void gCP::GradientCrystalPlasticitySolver<3>::copy_local_to_global_projection_rhs(
  const gCP::AssemblyData::Postprocessing::ProjectionRHS::Copy &);

template double gCP::GradientCrystalPlasticitySolver<2>::get_macroscopic_damage();
template double gCP::GradientCrystalPlasticitySolver<3>::get_macroscopic_damage();
