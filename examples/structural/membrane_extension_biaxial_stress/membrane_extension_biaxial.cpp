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
#include "examples/structural/membrane_extension_biaxial_stress/membrane_extension_biaxial.h"
#include "elasticity/structural_system_initialization.h"
#include "elasticity/structural_element_base.h"
#include "elasticity/structural_nonlinear_assembly.h"
#include "elasticity/structural_discipline.h"
#include "elasticity/stress_output_base.h"
#include "base/parameter.h"
#include "base/constant_field_function.h"
#include "property_cards/solid_2d_section_element_property_card.h"
#include "property_cards/isotropic_material_property_card.h"
#include "boundary_condition/dirichlet_boundary_condition.h"
#include "base/nonlinear_system.h"


// libMesh includes
#include "libmesh/mesh_generation.h"
#include "libmesh/exodusII_io.h"
#include "libmesh/numeric_vector.h"
#include "libmesh/parameter_vector.h"


extern libMesh::LibMeshInit* __init;


MAST::MembraneExtensionBiaxial::MembraneExtensionBiaxial():
_initialized(false) { }


void
MAST::MembraneExtensionBiaxial::init(libMesh::ElemType etype, bool if_nonlin) {

    libmesh_assert(!_initialized);
    
    libMesh::ElemType
    e_type       = libMesh::QUAD4;

    // length of domain
    _length     = 0.50,
    _width      = 0.25;
    
    
    // create the mesh
    _mesh       = new libMesh::SerialMesh(__init->comm());
    
    // initialize the mesh with one element
    libMesh::MeshTools::Generation::build_square(*_mesh,
                                                 16, 16,
                                                 0, _length,
                                                 0, _width,
                                                 e_type);
    
    // create the equation system
    _eq_sys    = new  libMesh::EquationSystems(*_mesh);
    
    // create the libmesh system
    _sys       = &(_eq_sys->add_system<MAST::NonlinearSystem>("structural"));
    
    // FEType to initialize the system
    libMesh::FEType fetype (libMesh::FIRST, libMesh::LAGRANGE);
    
    // initialize the system to the right set of variables
    _structural_sys = new MAST::StructuralSystemInitialization(*_sys,
                                                               _sys->name(),
                                                               fetype);
    _discipline     = new MAST::StructuralDiscipline(*_eq_sys);

    
    // create and add the boundary condition and loads
    _dirichlet_left   = new MAST::DirichletBoundaryCondition;
    _dirichlet_left->init   (3, _structural_sys->vars());
    _discipline->add_dirichlet_bc(3,   *_dirichlet_left);

    _discipline->init_system_dirichlet_bc(*_sys);
    
    // initialize the equation system
    _eq_sys->init();
    
    // create the property functions and add them to the
    
    _th              = new MAST::Parameter("th",    0.006);
    _E               = new MAST::Parameter("E",     72.e9);
    _nu              = new MAST::Parameter("nu",     0.33);
    _kappa           = new MAST::Parameter("kappa", 5./6.);
    _zero            = new MAST::Parameter("zero",     0.);
    _press           = new MAST::Parameter( "p",     2.e9);
    
    
    
    // prepare the vector of parameters with respect to which the sensitivity
    // needs to be benchmarked
    _params_for_sensitivity.push_back(_E);
    _params_for_sensitivity.push_back(_nu);
    _params_for_sensitivity.push_back(_th);
    
    
    
    _th_f            = new MAST::ConstantFieldFunction("h",           *_th);
    _E_f             = new MAST::ConstantFieldFunction("E",            *_E);
    _nu_f            = new MAST::ConstantFieldFunction("nu",          *_nu);
    _kappa_f         = new MAST::ConstantFieldFunction("kappa",    *_kappa);
    _hoff_f          = new MAST::ConstantFieldFunction("off",       *_zero);
    _press_f         = new MAST::ConstantFieldFunction("pressure", *_press);
    
    // initialize the load
    _p_load          = new MAST::BoundaryConditionBase(MAST::SURFACE_PRESSURE);
    _p_load->add(*_press_f);
    _discipline->add_side_load(1, *_p_load);
    
    // create the material property card
    _m_card         = new MAST::IsotropicMaterialPropertyCard;
    
    // add the material properties to the card
    _m_card->add(*_E_f);
    _m_card->add(*_nu_f);
    _m_card->add(*_kappa_f);
    
    // create the element property card
    _p_card         = new MAST::Solid2DSectionElementPropertyCard;
    
    // add the section properties to the card
    _p_card->add(*_th_f);
    _p_card->add(*_hoff_f);
    
    // tell the section property about the material property
    _p_card->set_material(*_m_card);
    
    _discipline->set_property_for_subdomain(0, *_p_card);
    
    
    // create the output objects, one for each element
    libMesh::MeshBase::const_element_iterator
    e_it    = _mesh->elements_begin(),
    e_end   = _mesh->elements_end();
    
    // points where stress is evaluated
    std::vector<libMesh::Point> pts;

    if (e_type == libMesh::QUAD4 ||
        e_type == libMesh::QUAD8 ||
        e_type == libMesh::QUAD9) {
        
        pts.push_back(libMesh::Point(-1/sqrt(3), -1/sqrt(3), 1.)); // upper skin
        pts.push_back(libMesh::Point(-1/sqrt(3), -1/sqrt(3),-1.)); // lower skin
        pts.push_back(libMesh::Point( 1/sqrt(3), -1/sqrt(3), 1.)); // upper skin
        pts.push_back(libMesh::Point( 1/sqrt(3), -1/sqrt(3),-1.)); // lower skin
        pts.push_back(libMesh::Point( 1/sqrt(3),  1/sqrt(3), 1.)); // upper skin
        pts.push_back(libMesh::Point( 1/sqrt(3),  1/sqrt(3),-1.)); // lower skin
        pts.push_back(libMesh::Point(-1/sqrt(3),  1/sqrt(3), 1.)); // upper skin
        pts.push_back(libMesh::Point(-1/sqrt(3),  1/sqrt(3),-1.)); // lower skin
    }
    else if (e_type == libMesh::TRI3 ||
             e_type == libMesh::TRI6) {
        
        pts.push_back(libMesh::Point(1./3., 1./3., 1.)); // upper skin
        pts.push_back(libMesh::Point(1./3., 1./3.,-1.)); // lower skin
        pts.push_back(libMesh::Point(2./3., 1./3., 1.)); // upper skin
        pts.push_back(libMesh::Point(2./3., 1./3.,-1.)); // lower skin
        pts.push_back(libMesh::Point(1./3., 2./3., 1.)); // upper skin
        pts.push_back(libMesh::Point(1./3., 2./3.,-1.)); // lower skin
    }
    else
        libmesh_assert(false); // should not get here

    for ( ; e_it != e_end; e_it++) {
        
        MAST::StressStrainOutputBase * output = new MAST::StressStrainOutputBase;
        
        // tell the object to evaluate the data for this object only
        std::set<const libMesh::Elem*> e_set;
        e_set.insert(*e_it);
        output->set_elements_in_domain(e_set);
        output->set_points_for_evaluation(pts);
        output->set_volume_loads(_discipline->volume_loads());
        _outputs.push_back(output);
        
        _discipline->add_volume_output((*e_it)->subdomain_id(), *output);
    }
    
    _initialized = true;
}







MAST::MembraneExtensionBiaxial::~MembraneExtensionBiaxial() {
    
    if (!_initialized)
        return;
    
    delete _m_card;
    delete _p_card;
    
    delete _p_load;
    delete _dirichlet_left;
    
    delete _th_f;
    delete _E_f;
    delete _nu_f;
    delete _kappa_f;
    delete _hoff_f;
    delete _press_f;
    
    delete _th;
    delete _E;
    delete _nu;
    delete _kappa;
    delete _zero;
    delete _press;
    
    
    
    delete _eq_sys;
    delete _mesh;
    
    delete _discipline;
    delete _structural_sys;
    
    // iterate over the output quantities and delete them
    std::vector<MAST::StressStrainOutputBase*>::iterator
    it   =   _outputs.begin(),
    end  =   _outputs.end();
    
    for ( ; it != end; it++)
        delete *it;
    
    _outputs.clear();
}



MAST::Parameter*
MAST::MembraneExtensionBiaxial::get_parameter(const std::string &nm) {
    
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



const libMesh::NumericVector<Real>&
MAST::MembraneExtensionBiaxial::solve(bool if_write_output) {
    
    libmesh_assert(_initialized);
    
    // create the nonlinear assembly object
    MAST::StructuralNonlinearAssembly   assembly;
    
    assembly.attach_discipline_and_system(*_discipline, *_structural_sys);
    
    MAST::NonlinearSystem& nonlin_sys = assembly.system();
    
    // zero the solution before solving
    nonlin_sys.solution->zero();
    this->clear_stresss();
    
    nonlin_sys.solve();
    
    // evaluate the outputs
    assembly.calculate_outputs(*(_sys->solution));

    assembly.clear_discipline_and_system();
    
    if (if_write_output) {
        
        libMesh::out << "Writing output to : output.exo" << std::endl;
        
        // write the solution for visualization
        libMesh::ExodusII_IO(*_mesh).write_equation_systems("output.exo",
                                                            *_eq_sys);
        
        _discipline->plot_stress_strain_data<libMesh::ExodusII_IO>("stress_output.exo");
    }
    
    return *(_sys->solution);
}





const libMesh::NumericVector<Real>&
MAST::MembraneExtensionBiaxial::sensitivity_solve(MAST::Parameter& p,
                                     bool if_write_output) {
    
    libmesh_assert(_initialized);
    
    _discipline->add_parameter(p);
    
    // create the nonlinear assembly object
    MAST::StructuralNonlinearAssembly   assembly;
    
    assembly.attach_discipline_and_system(*_discipline, *_structural_sys);

    MAST::NonlinearSystem& nonlin_sys = assembly.system();

    libMesh::ParameterVector params;
    params.resize(1);
    params[0]  =  p.ptr();

    // zero the solution before solving
    nonlin_sys.add_sensitivity_solution(0).zero();
    this->clear_stresss();
    
    nonlin_sys.sensitivity_solve(params);
    
    // evaluate sensitivity of the outputs
    assembly.calculate_output_sensitivity(params,
                                          true,    // true for total sensitivity
                                          *(_sys->solution));
    
    
    assembly.clear_discipline_and_system();
    _discipline->remove_parameter(p);
    
    // write the solution for visualization
    if (if_write_output) {
        
        std::ostringstream oss1, oss2;
        oss1 << "output_" << p.name() << ".exo";
        oss2 << "output_" << p.name() << ".exo";
        
        libMesh::out
        << "Writing sensitivity output to : " << oss1.str()
        << "  and stress/strain sensitivity to : " << oss2.str()
        << std::endl;
        
        
        _sys->solution->swap(_sys->get_sensitivity_solution(0));
        
        // write the solution for visualization
        libMesh::ExodusII_IO(*_mesh).write_equation_systems(oss1.str(),
                                                            *_eq_sys);
        _discipline->plot_stress_strain_data<libMesh::ExodusII_IO>(oss2.str(), &p);

        _sys->solution->swap(_sys->get_sensitivity_solution(0));
    }
    
    return _sys->get_sensitivity_solution(0);
}



void
MAST::MembraneExtensionBiaxial::clear_stresss() {
    
    // iterate over the output quantities and delete them
    std::vector<MAST::StressStrainOutputBase*>::iterator
    it   =   _outputs.begin(),
    end  =   _outputs.end();
    
    for ( ; it != end; it++)
        (*it)->clear(false);
}



