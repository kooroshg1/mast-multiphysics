/*
 * MAST: Multidisciplinary-design Adaptation and Sensitivity Toolkit
 * Copyright (C) 2013-2017  Manav Bhatia
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

// C++ includes
#include <iostream>


// MAST includes
#include "examples/structural/beam_piston_theory_flutter/beam_piston_theory_flutter.h"
#include "base/nonlinear_system.h"
#include "base/parameter.h"
#include "base/constant_field_function.h"
#include "elasticity/structural_system_initialization.h"
#include "elasticity/structural_discipline.h"
#include "elasticity/structural_element_base.h"
#include "elasticity/structural_modal_eigenproblem_assembly.h"
#include "elasticity/structural_fluid_interaction_assembly.h"
#include "elasticity/piston_theory_boundary_condition.h"
#include "aeroelasticity/time_domain_flutter_solver.h"
#include "aeroelasticity/flutter_root_base.h"
#include "property_cards/solid_1d_section_element_property_card.h"
#include "property_cards/isotropic_material_property_card.h"
#include "boundary_condition/dirichlet_boundary_condition.h"
#include "solver/slepc_eigen_solver.h"
#include "examples/base/plot_results.h"


// libMesh includes
#include "libmesh/mesh_generation.h"
#include "libmesh/exodusII_io.h"
#include "libmesh/numeric_vector.h"

extern libMesh::LibMeshInit* __init;


MAST::BeamPistonTheoryFlutterAnalysis::BeamPistonTheoryFlutterAnalysis():
_initialized(false),
_flutter_root(nullptr) { }



void
MAST::BeamPistonTheoryFlutterAnalysis::init(libMesh::ElemType etype,
                                            bool if_nonlin) {
    
    libmesh_assert(!_initialized);
    
    // create the mesh
    _mesh       = new libMesh::SerialMesh(__init->comm());
    _length     = 10.;
    
    // initialize the mesh with one element
    libMesh::MeshTools::Generation::build_line(*_mesh, 50, 0, _length);
    
    // create the equation system
    _eq_sys    = new  libMesh::EquationSystems(*_mesh);
    
    // create the libmesh system
    _sys       = &(_eq_sys->add_system<MAST::NonlinearSystem>("structural"));
    _sys->set_eigenproblem_type(libMesh::GHEP);
    
    // FEType to initialize the system
    libMesh::FEType fetype (libMesh::FIRST, libMesh::LAGRANGE);
    
    // initialize the system to the right set of variables
    _structural_sys = new MAST::StructuralSystemInitialization(*_sys,
                                                               _sys->name(),
                                                               fetype);
    _discipline     = new MAST::StructuralDiscipline(*_eq_sys);
    
    
    // create and add the boundary condition and loads
    _dirichlet_left = new MAST::DirichletBoundaryCondition;
    _dirichlet_right= new MAST::DirichletBoundaryCondition;
    std::vector<unsigned int> constrained_vars(4);
    constrained_vars[0] = 0;  // u
    constrained_vars[1] = 1;  // v
    constrained_vars[2] = 2;  // w
    constrained_vars[3] = 3;  // tx
    _dirichlet_left->init (0, constrained_vars);
    _dirichlet_right->init(1, constrained_vars);
    _discipline->add_dirichlet_bc(0, *_dirichlet_left);
    _discipline->add_dirichlet_bc(1, *_dirichlet_right);
    _discipline->init_system_dirichlet_bc(*_sys);
    
    // initialize the equation system
    _eq_sys->init();
    
    _sys->eigen_solver->set_position_of_spectrum(libMesh::LARGEST_MAGNITUDE);
    _sys->set_exchange_A_and_B(true);
    _sys->set_n_requested_eigenvalues(3);
    
    // create the property functions and add them to the
    
    _thy             = new MAST::Parameter("thy",  0.06);
    _thz             = new MAST::Parameter("thz",  1.00);
    _rho             = new MAST::Parameter("rho", 2.8e3);
    _E               = new MAST::Parameter("E",   72.e9);
    _nu              = new MAST::Parameter("nu",   0.33);
    _zero            = new MAST::Parameter("zero",   0.);
    _velocity        = new MAST::Parameter("V"   ,   0.);
    _mach            = new MAST::Parameter("mach",   3.);
    _rho_air         = new MAST::Parameter("rho" , 1.05);
    _gamma_air       = new MAST::Parameter("gamma", 1.4);
    
    
    
    // prepare the vector of parameters with respect to which the sensitivity
    // needs to be benchmarked
    _params_for_sensitivity.push_back(_E);
    _params_for_sensitivity.push_back(_nu);
    _params_for_sensitivity.push_back(_thy);
    _params_for_sensitivity.push_back(_thz);
    
    
    
    _thy_f           = new MAST::ConstantFieldFunction("hy",          *_thy);
    _thz_f           = new MAST::ConstantFieldFunction("hz",          *_thz);
    _rho_f           = new MAST::ConstantFieldFunction("rho",         *_rho);
    _E_f             = new MAST::ConstantFieldFunction("E",             *_E);
    _nu_f            = new MAST::ConstantFieldFunction("nu",           *_nu);
    _hyoff_f         = new MAST::ConstantFieldFunction("hy_off",     *_zero);
    _hzoff_f         = new MAST::ConstantFieldFunction("hz_off",     *_zero);
    _velocity_f      = new MAST::ConstantFieldFunction("V",      *_velocity);
    _mach_f          = new MAST::ConstantFieldFunction("mach",       *_mach);
    _rho_air_f       = new MAST::ConstantFieldFunction("rho",     *_rho_air);
    _gamma_air_f     = new MAST::ConstantFieldFunction("gamma", *_gamma_air);
    
    // create the material property card
    _m_card          = new MAST::IsotropicMaterialPropertyCard;
    
    // add the material properties to the card
    _m_card->add(*_rho_f);
    _m_card->add(*_E_f);
    _m_card->add(*_nu_f);
    
    // create the element property card
    _p_card          = new MAST::Solid1DSectionElementPropertyCard;
    
    // tell the card about the orientation
    libMesh::Point orientation;
    orientation(1) = 1.;
    _p_card->y_vector() = orientation;
    
    // add the section properties to the card
    _p_card->add(*_thy_f);
    _p_card->add(*_thz_f);
    _p_card->add(*_hyoff_f);
    _p_card->add(*_hzoff_f);
    
    // tell the section property about the material property
    _p_card->set_material(*_m_card);
    
    _p_card->init();
    
    _discipline->set_property_for_subdomain(0, *_p_card);
    
    // now initialize the piston theory boundary conditions
    RealVectorX  vel = RealVectorX::Zero(3);
    vel(0)           = 1.;  // flow along the x-axis
    _piston_bc       = new MAST::PistonTheoryBoundaryCondition(1,     // order
                                                               vel);  // vel vector
    _piston_bc->add(*_velocity_f);
    _piston_bc->add(*_mach_f);
    _piston_bc->add(*_rho_air_f);
    _piston_bc->add(*_gamma_air_f);
    _discipline->add_volume_load(0, *_piston_bc);
    
    
    // initialize the flutter solver
    _flutter_solver  = new MAST::TimeDomainFlutterSolver;
    
    _initialized = true;
}







MAST::BeamPistonTheoryFlutterAnalysis::~BeamPistonTheoryFlutterAnalysis() {
    
    if (!_initialized)
        return;
    
    delete _m_card;
    delete _p_card;
    
    delete _dirichlet_left;
    delete _dirichlet_right;
    
    delete _thy_f;
    delete _thz_f;
    delete _rho_f;
    delete _E_f;
    delete _nu_f;
    delete _hyoff_f;
    delete _hzoff_f;
    delete _velocity_f;
    delete _mach_f;
    delete _rho_air_f;
    delete _gamma_air_f;

    
    delete _thy;
    delete _thz;
    delete _rho;
    delete _E;
    delete _nu;
    delete _zero;
    delete _velocity;
    delete _mach;
    delete _rho_air;
    delete _gamma_air;
    
    
    // delete the basis vectors
    if (_basis.size())
        for (unsigned int i=0; i<_basis.size(); i++)
            delete _basis[i];
    
    
    delete _eq_sys;
    delete _mesh;
    
    delete _discipline;
    delete _structural_sys;
    
    delete _flutter_solver;
    delete _piston_bc;
}




MAST::Parameter*
MAST::BeamPistonTheoryFlutterAnalysis::get_parameter(const std::string &nm) {
    
    MAST::Parameter *rval = nullptr;
    
    // look through the vector of parameters to see if the name is available
    std::vector<MAST::Parameter*>::iterator
    it   =  _params_for_sensitivity.begin(),
    end  =  _params_for_sensitivity.end();
    
    bool
    found = false;
    
    for ( ; it != end; it++) {
        
        if (nm == (*it)->name()) {
            rval    = *it;
            found   = true;
        }
    }
    
    // if the param was not found, then print the message
    if (!found) {
        libMesh::out
        << std::endl
        << "Parameter not found by name: " << nm << std::endl
        << "Valid names are: "
        << std::endl;
        for (it = _params_for_sensitivity.begin(); it != end; it++)
            libMesh::out << "   " << (*it)->name() << std::endl;
        libMesh::out << std::endl;
    }
    
    return rval;
}



Real
MAST::BeamPistonTheoryFlutterAnalysis::solve(bool if_write_output,
                                             const Real tol,
                                             const unsigned int max_bisection_iters) {
    
    libmesh_assert(_initialized);
    
    // clear out the data structures of the flutter solver before
    // this solution
    _flutter_root = nullptr;
    _flutter_solver->clear();
    std::string nm("flutter_output.txt");
    if (__init->comm().rank() == 0)
        _flutter_solver->set_output_file(nm);

    
    // set the velocity of piston theory to zero for modal analysis
    (*_velocity) = 0.;

    // create the nonlinear assembly object
    MAST::StructuralModalEigenproblemAssembly   assembly;
    _sys->initialize_condensed_dofs(*_discipline);
    
    assembly.attach_discipline_and_system(*_discipline, *_structural_sys);
    _sys->eigenproblem_solve();
    assembly.clear_discipline_and_system();
    
    // Get the number of converged eigen pairs.
    unsigned int
    nconv = std::min(_sys->get_n_converged_eigenvalues(),
                     _sys->get_n_requested_eigenvalues());
    
    if (_basis.size() > 0)
        libmesh_assert(_basis.size() == nconv);
    else {
        _basis.resize(nconv);
        for (unsigned int i=0; i<_basis.size(); i++)
            _basis[i] = nullptr;
    }
    
    libMesh::ExodusII_IO*
    writer = nullptr;
    
    if (if_write_output)
        writer = new libMesh::ExodusII_IO(*_mesh);

    for (unsigned int i=0; i<nconv; i++) {
        
        // create a vector to store the basis
        if (_basis[i] == nullptr)
            _basis[i] = _sys->solution->zero_clone().release();
        
        // now write the eigenvalue
        Real
        re = 0.,
        im = 0.;
        _sys->get_eigenpair(i, re, im, *_basis[i]);
        
        libMesh::out
        << std::setw(35) << std::fixed << std::setprecision(15)
        << re << std::endl;
        
        if (if_write_output) {
            
            // copy the solution for output
            (*_sys->solution) = *_basis[i];
            
            // We write the file in the ExodusII format.
            writer->write_timestep("modes.exo",
                                   *_eq_sys,
                                   i+1, i);
        }
    }
    
    // now initialize the flutter solver
    MAST::StructuralFluidInteractionAssembly fsi_assembly;
    fsi_assembly.attach_discipline_and_system(*_discipline,
                                              *_structural_sys);
    _flutter_solver->attach_assembly(fsi_assembly);
    _flutter_solver->initialize(*_velocity,
                                1.e3,        // lower V
                                1200.,         // upper V
                                10,           // number of divisions
                                _basis);      // basis vectors

    std::pair<bool, MAST::FlutterRootBase*>
    sol = _flutter_solver->analyze_and_find_critical_root_without_tracking(tol, max_bisection_iters);
    _flutter_solver->print_sorted_roots();
    fsi_assembly.clear_discipline_and_system();
    _flutter_solver->clear_assembly_object();

    // make sure solution was found
    libmesh_assert(sol.first);
    _flutter_root = sol.second;
    
    
    if (sol.first && if_write_output) {
        
        MAST::plot_structural_flutter_solution("structural_flutter_mode.exo",
                                               *_sys,
                                               sol.second->eig_vec_right,
                                               _basis);
    }
    
    
    return _flutter_root->V;
}





Real
MAST::BeamPistonTheoryFlutterAnalysis::
sensitivity_solve(MAST::Parameter& p) {
    
    libmesh_assert(_initialized);
    
    //Make sure that  a solution is available for sensitivity
    libmesh_assert(_flutter_root);
    
    // flutter solver will need velocity to be defined as a parameter for
    // sensitivity analysis
    _discipline->add_parameter(*_velocity);
    _discipline->add_parameter(p);
    
    libMesh::ParameterVector params;
    params.resize(1);
    params[0]  =  p.ptr();
    
    // initialize the flutter solver for sensitivity.
    MAST::StructuralFluidInteractionAssembly fsi_assembly;
    fsi_assembly.attach_discipline_and_system(*_discipline, *_structural_sys);
    _flutter_solver->attach_assembly(fsi_assembly);
    _flutter_solver->calculate_sensitivity(*_flutter_root, params, 0);
    fsi_assembly.clear_discipline_and_system();
    _flutter_solver->clear_assembly_object();
    
    _discipline->remove_parameter(p);
    _discipline->remove_parameter(*_velocity);
    return _flutter_root->V_sens;
}


