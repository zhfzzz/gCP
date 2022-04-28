#include <gCP/assembly_data.h>



namespace gCP
{



namespace AssemblyData
{



CopyBase::CopyBase(const unsigned int dofs_per_cell)
:
dofs_per_cell(dofs_per_cell),
local_dof_indices(dofs_per_cell)
{}



template <int dim>
ScratchBase<dim>::ScratchBase(
  const dealii::hp::QCollection<dim>  &quadrature_collection,
  const dealii::hp::FECollection<dim> &finite_element_collection)
:
n_q_points(quadrature_collection.max_n_quadrature_points()),
dofs_per_cell(finite_element_collection.max_dofs_per_cell())
{}



template <int dim>
ScratchBase<dim>::ScratchBase(const ScratchBase<dim> &data)
:
n_q_points(data.n_q_points),
dofs_per_cell(data.dofs_per_cell)
{}



namespace Jacobian
{



Copy::Copy(const unsigned int dofs_per_cell)
:
CopyBase(dofs_per_cell),
local_matrix(dofs_per_cell, dofs_per_cell)
{}



template <int dim>
Scratch<dim>::Scratch(
  const dealii::hp::MappingCollection<dim>  &mapping,
  const dealii::hp::QCollection<dim>        &quadrature_collection,
  const dealii::hp::FECollection<dim>       &finite_element_collection,
  const dealii::UpdateFlags                 update_flags)
:
ScratchBase<dim>(quadrature_collection, finite_element_collection),
hp_fe_values(mapping,
             finite_element_collection,
             quadrature_collection,
             update_flags),
sym_grad_phi(this->dofs_per_cell)
{}



template <int dim>
Scratch<dim>::Scratch(const Scratch<dim> &data)
:
ScratchBase<dim>(data),
hp_fe_values(data.hp_fe_values.get_mapping_collection(),
             data.hp_fe_values.get_fe_collection(),
             data.hp_fe_values.get_quadrature_collection(),
             data.hp_fe_values.get_update_flags()),
sym_grad_phi(this->dofs_per_cell)
{}



} // namespace Jacobian



namespace Residual
{



Copy::Copy(const unsigned int dofs_per_cell)
:
CopyBase(dofs_per_cell),
local_rhs(dofs_per_cell),
local_matrix_for_inhomogeneous_bcs(dofs_per_cell, dofs_per_cell)
{}



template <int dim>
Scratch<dim>::Scratch(
  const dealii::hp::MappingCollection<dim>  &mapping,
  const dealii::hp::QCollection<dim>        &quadrature_collection,
  const dealii::hp::QCollection<dim-1>      &face_quadrature_collection,
  const dealii::hp::FECollection<dim>       &finite_element_collection,
  const dealii::UpdateFlags                 update_flags,
  const dealii::UpdateFlags                 face_update_flags)
:
ScratchBase<dim>(quadrature_collection, finite_element_collection),
hp_fe_values(mapping,
             finite_element_collection,
             quadrature_collection,
             update_flags),
hp_fe_face_values(mapping,
                  finite_element_collection,
                  face_quadrature_collection,
                  face_update_flags),
n_face_q_points(face_quadrature_collection.max_n_quadrature_points()),
phi(this->dofs_per_cell),
sym_grad_phi(this->dofs_per_cell),
face_phi(this->dofs_per_cell),
strain_tensor_values(this->n_q_points),
stress_tensor_values(this->n_q_points),
supply_term_values(this->n_q_points),
neumann_boundary_values(n_face_q_points)
{}



template <int dim>
Scratch<dim>::Scratch(const Scratch<dim> &data)
:
ScratchBase<dim>(data),
hp_fe_values(data.hp_fe_values.get_mapping_collection(),
             data.hp_fe_values.get_fe_collection(),
             data.hp_fe_values.get_quadrature_collection(),
             data.hp_fe_values.get_update_flags()),
hp_fe_face_values(data.hp_fe_face_values.get_mapping_collection(),
                  data.hp_fe_face_values.get_fe_collection(),
                  data.hp_fe_face_values.get_quadrature_collection(),
                  data.hp_fe_face_values.get_update_flags()),
n_face_q_points(data.n_face_q_points),
phi(this->dofs_per_cell),
sym_grad_phi(this->dofs_per_cell),
face_phi(this->dofs_per_cell),
strain_tensor_values(this->n_q_points),
stress_tensor_values(this->n_q_points),
supply_term_values(this->n_q_points),
neumann_boundary_values(n_face_q_points)
{}



} // namespace Residual



} // namespace AssemblyData



} // namespace gCP



template struct gCP::AssemblyData::ScratchBase<2>;
template struct gCP::AssemblyData::ScratchBase<3>;

template struct gCP::AssemblyData::Jacobian::Scratch<2>;
template struct gCP::AssemblyData::Jacobian::Scratch<3>;

template struct gCP::AssemblyData::Residual::Scratch<2>;
template struct gCP::AssemblyData::Residual::Scratch<3>;