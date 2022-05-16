#include <gCP/constitutive_laws.h>

#include <deal.II/base/symmetric_tensor.h>

namespace gCP
{



namespace Kinematics
{



template <int dim>
ElasticStrain<dim>::ElasticStrain(
  std::shared_ptr<CrystalsData<dim>>  crystals_data)
:
crystals_data(crystals_data),
flag_init_was_called(false)
{}



template <int dim>
void ElasticStrain<dim>::init(ExtractorPair &extractor_pair)
{
  displacements_extractors  = extractor_pair.first;
  slips_extractors          = extractor_pair.second;

  flag_init_was_called = true;
}



template <int dim>
const std::vector<dealii::SymmetricTensor<2,dim>>
ElasticStrain<dim>::get_elastic_strain_tensor(
  const dealii::LinearAlgebraTrilinos::MPI::Vector  solution,
  const dealii::FEValues<dim>                       &fe_values,
  const dealii::types::material_id                  crystal_id) const
{
  AssertThrow(crystals_data->is_initialized(),
              dealii::ExcMessage("The underlying CrystalsData<dim>"
                                  " instance has not been "
                                  " initialized."));

  AssertThrow(flag_init_was_called,
              dealii::ExcMessage("The ElasticStrain<dim> instance"
                                  " has not been initialized."));

  const unsigned int n_q_points = fe_values.n_quadrature_points;

  std::vector<dealii::SymmetricTensor<2,dim>> strain_tensor_values(
    n_q_points,
    dealii::SymmetricTensor<2,dim>());

  std::vector<dealii::SymmetricTensor<2,dim>> elastic_strain_tensor_values(
    n_q_points,
    dealii::SymmetricTensor<2,dim>());

  dealii::SymmetricTensor<2,dim>              symmetrized_schmid_tensor;

  std::vector<double>                         slip_values(n_q_points, 0);

  fe_values[displacements_extractors[crystal_id]].get_function_symmetric_gradients(
    solution,
    strain_tensor_values);

  elastic_strain_tensor_values  = strain_tensor_values;

  for (unsigned int slip_id = 0;
       slip_id < crystals_data->get_n_slips();
       ++slip_id)
  {
    fe_values[slips_extractors[crystal_id][slip_id]].get_function_values(
      solution,
      slip_values);

    symmetrized_schmid_tensor =
      crystals_data->get_symmetrized_schmid_tensor(crystal_id, slip_id);

    for (unsigned int q_point = 0; q_point < n_q_points; ++q_point)
    {
      elastic_strain_tensor_values[q_point] -=
        slip_values[q_point] * symmetrized_schmid_tensor;
    }
  }

  return elastic_strain_tensor_values;
}



} // namespace Kinematics



namespace ConstitutiveLaws
{



template<int dim>
HookeLaw<dim>::HookeLaw(const double youngs_modulus,
                        const double poissons_ratio)
:
crystal_system(CrystalSystem::Isotropic),
C_1111(youngs_modulus * poissons_ratio /
       ((1.0 + poissons_ratio) * (1.0 - 2.0 * poissons_ratio))
       +
       youngs_modulus / (1.0 + poissons_ratio)),
C_1212(youngs_modulus / 2.0 / (1.0 + poissons_ratio)),
C_1122(youngs_modulus * poissons_ratio /
       ((1.0 + poissons_ratio) * (1.0 - 2.0 * poissons_ratio))),
flag_init_was_called(false)
{}



template<int dim>
HookeLaw<dim>::HookeLaw(
  const std::shared_ptr<CrystalsData<dim>> &crystals_data,
  const double                             C_1111,
  const double                             C_1212,
  const double                             C_1122)
:
crystals_data(crystals_data),
crystal_system(CrystalSystem::Cubic),
C_1111(C_1111),
C_1212(C_1212),
C_1122(C_1122),
flag_init_was_called(false)
{}



template<int dim>
void HookeLaw<dim>::init()
{
  for (unsigned int i = 0; i < dim; i++)
    for (unsigned int j = 0; j < dim; j++)
      for (unsigned int k = 0; k < dim; k++)
        for (unsigned int l = 0; l < dim; l++)
          if (i == j && j == k && k == l)
            reference_stiffness_tetrad[i][j][k][l] = C_1111;
          else if (i == k && j == l)
            reference_stiffness_tetrad[i][j][k][l] = C_1212;
          else if (i == j && k == l)
            reference_stiffness_tetrad[i][j][k][l] = C_1122;

  switch (crystal_system)
  {
  case CrystalSystem::Isotropic:
    break;

  case CrystalSystem::Cubic:
    {
      AssertThrow(crystals_data->is_initialized(),
                  dealii::ExcMessage("The underlying CrystalsData<dim>"
                                     " instance has not been "
                                     " initialized."));

      for (unsigned int crystal_id = 0;
           crystal_id < crystals_data->get_n_crystals();
           crystal_id++)
        {
          dealii::SymmetricTensor<4,dim> stiffness_tetrad;

          dealii::Tensor<2,dim> rotation_tensor =
            crystals_data->get_rotation_tensor(crystal_id);

          for (unsigned int i = 0; i < dim; i++)
            for (unsigned int j = 0; j < dim; j++)
              for (unsigned int k = 0; k < dim; k++)
                for (unsigned int l = 0; l < dim; l++)
                  for (unsigned int o = 0; o < dim; o++)
                    for (unsigned int p = 0; p < dim; p++)
                      for (unsigned int q = 0; q < dim; q++)
                        for (unsigned int r = 0; r < dim; r++)
                          stiffness_tetrad[i][j][k][l] =
                            rotation_tensor[i][o] *
                            rotation_tensor[j][p] *
                            rotation_tensor[k][q] *
                            rotation_tensor[l][r] *
                            reference_stiffness_tetrad[o][p][q][r];

          stiffness_tetrads.push_back(stiffness_tetrad);
        }
    }
    break;

  default:
    break;
  }

  flag_init_was_called = true;
}


template<int dim>
const dealii::SymmetricTensor<2,dim> HookeLaw<dim>::
get_stress_tensor(
  const dealii::SymmetricTensor<2,dim> strain_tensor_values) const
{
  AssertThrow(flag_init_was_called,
              dealii::ExcMessage("The HookeLaw<dim> instance has not"
                                 " been initialized."));

  return reference_stiffness_tetrad * strain_tensor_values;
}



template<int dim>
const dealii::SymmetricTensor<2,dim> HookeLaw<dim>::
get_stress_tensor(
  const unsigned int                    crystal_id,
  const dealii::SymmetricTensor<2,dim>  strain_tensor_values) const
{
  AssertThrow(crystals_data.get() != nullptr,
              dealii::ExcMessage("This overloaded method requires a "
                                 "constructor called where a "
                                 "CrystalsData<dim> instance is "
                                 "passed as a std::shared_ptr"))

  AssertThrow(flag_init_was_called,
              dealii::ExcMessage("The HookeLaw<dim> instance has not"
                                 " been initialized."));

  AssertIndexRange(crystal_id, crystals_data->get_n_crystals());

  return stiffness_tetrads[crystal_id] * strain_tensor_values;
}



template<int dim>
ScalarMicroscopicStressLaw<dim>::ScalarMicroscopicStressLaw(
  const std::shared_ptr<CrystalsData<dim>> &crystals_data,
  const std::string                        regularization_function,
  const double                             regularization_parameter,
  const double                             initial_slip_resistance,
  const double                             linear_hardening_modulus,
  const double                             hardening_parameter)
:
crystals_data(crystals_data),
regularization_parameter(regularization_parameter),
initial_slip_resistance(initial_slip_resistance),
linear_hardening_modulus(linear_hardening_modulus),
hardening_parameter(hardening_parameter),
flag_init_was_called(false)
{
  if (regularization_function == "power_law")
    this->regularization_function = RegularizationFunction::PowerLaw;
  else if (regularization_function == "tanh")
    this->regularization_function = RegularizationFunction::Tanh;
  else
    AssertThrow(false, dealii::ExcMessage("The given regularization "
                                          "function is not currently "
                                          "implemented."))
}



template<int dim>
double ScalarMicroscopicStressLaw<dim>::get_scalar_microscopic_stress(
  const double crystal_id,
  const double slip_id,
  const double slip_rate)
{
  double regularization_factor;

  switch (regularization_function)
  {
  case RegularizationFunction::PowerLaw:
    {
      regularization_factor = std::pow(slip_rate,
                                       1.0 / regularization_parameter);
    }
    break;
  case RegularizationFunction::Tanh:
    {
      regularization_factor = std::tanh(slip_rate /
                                        regularization_parameter);
    }
    break;
  default:
    {
      AssertThrow(false, dealii::ExcMessage("The given regularization "
                                            "function is not currently "
                                            "implemented."));
    }
    break;
  }

  return ((initial_slip_resistance +
           hardening_field_at_q_points[crystal_id][slip_id]) *
           regularization_factor);
}



template<int dim>
double ScalarMicroscopicStressLaw<dim>::
  get_gateaux_derivative(const unsigned int  crystald_id,
                         const unsigned int  slip_id,
                         const bool          self_hardening,
                         const double        slip_rate_alpha,
                         const double        slip_rate_beta,
                         const double        time_step_size)
{
  auto sgn = [](const double value)
  {
    return (0.0 < value) - (value < 0.0);
  };

  double regularization_factor = 0.0;

  switch (regularization_function)
  {
  case RegularizationFunction::PowerLaw:
    {
      regularization_factor = std::pow(slip_rate_alpha,
                                       1.0 /
                                        regularization_parameter);
    }
    break;
  case RegularizationFunction::Tanh:
    {
      regularization_factor = std::tanh(slip_rate_alpha /
                                        regularization_parameter);
    }
    break;
  default:
    {
      AssertThrow(false, dealii::ExcMessage("The given regularization "
                                            "function is not currently "
                                            "implemented."));
    }
    break;
  }

  double gateaux_derivate = get_hardening_matrix_entry(self_hardening) *
                            sgn(slip_rate_beta) *
                            regularization_factor;

  if (self_hardening)
    gateaux_derivate +=
      (initial_slip_resistance +
       hardening_field_at_q_points[crystald_id][slip_id] ) /
      (time_step_size *
       hardening_parameter) *
      std::pow(1.0/std::cosh(slip_rate_alpha), 2);

  return 0.0;
}



template<int dim>
VectorMicroscopicStressLaw<dim>::VectorMicroscopicStressLaw(
  const std::shared_ptr<CrystalsData<dim>> &crystals_data,
  const double                              energetic_length_scale,
  const double                              initial_slip_resistance)
:
crystals_data(crystals_data),
energetic_length_scale(energetic_length_scale),
initial_slip_resistance(initial_slip_resistance),
flag_init_was_called(false)
{}



template<int dim>
void VectorMicroscopicStressLaw<dim>::init()
{
  AssertThrow(crystals_data->is_initialized(),
              dealii::ExcMessage("The underlying CrystalsData<dim>"
                                  " instance has not been "
                                  " initialized."));

  for (unsigned int crystal_id = 0;
        crystal_id < crystals_data->get_n_crystals();
        crystal_id++)
  {
    std::vector<dealii::SymmetricTensor<2,dim>>
      reduced_gradient_hardening_tensors_per_crystal(
        crystals_data->get_n_slips(),
        dealii::SymmetricTensor<2,dim>());

    for (unsigned int slip_id;
          slip_id < crystals_data->get_n_slips();
          ++slip_id)
    {
      dealii::Tensor<1,dim> slip_direction =
        crystals_data->get_slip_direction(crystal_id, slip_id);

      dealii::Tensor<1,dim> slip_orthogonal =
        crystals_data->get_slip_orthogonal(crystal_id, slip_id);

      dealii::SymmetricTensor<2,dim> reduced_gradient_hardening_tensor =
        initial_slip_resistance *
        energetic_length_scale * energetic_length_scale *
        (dealii::symmetrize(dealii::outer_product(slip_direction,
                                                  slip_direction)) +
          dealii::symmetrize(dealii::outer_product(slip_orthogonal,
                                                   slip_orthogonal)));

      reduced_gradient_hardening_tensors_per_crystal.push_back(
        reduced_gradient_hardening_tensor);
    }

    reduced_gradient_hardening_tensors.push_back(
      reduced_gradient_hardening_tensors_per_crystal);
  }

  flag_init_was_called = true;
}



template <int dim>
dealii::Tensor<1,dim> VectorMicroscopicStressLaw<dim>::
get_vector_microscopic_stress(
  const unsigned int          crystal_id,
  const unsigned int          slip_id,
  const dealii::Tensor<1,dim> slip_gradient) const
{
  AssertThrow(flag_init_was_called,
              dealii::ExcMessage("The VectorMicroscopicStressLaw<dim> "
                                 "instance has not been initialized."));

  return (reduced_gradient_hardening_tensors[crystal_id][slip_id] *
          slip_gradient);
}

} // ConstitutiveLaws



} // gCP


template class gCP::Kinematics::ElasticStrain<2>;
template class gCP::Kinematics::ElasticStrain<3>;

template class gCP::ConstitutiveLaws::HookeLaw<2>;
template class gCP::ConstitutiveLaws::HookeLaw<3>;

template class gCP::ConstitutiveLaws::ResolvedShearStressLaw<2>;
template class gCP::ConstitutiveLaws::ResolvedShearStressLaw<3>;

template class gCP::ConstitutiveLaws::ScalarMicroscopicStressLaw<2>;
template class gCP::ConstitutiveLaws::ScalarMicroscopicStressLaw<3>;

template class gCP::ConstitutiveLaws::VectorMicroscopicStressLaw<2>;
template class gCP::ConstitutiveLaws::VectorMicroscopicStressLaw<3>;