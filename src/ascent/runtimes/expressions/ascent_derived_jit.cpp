//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) 2015-2019, Lawrence Livermore National Security, LLC.
//
// Produced at the Lawrence Livermore National Laboratory
//
// LLNL-CODE-716457
//
// All rights reserved.
//
// This file is part of Ascent.
//
// For details, see: http://ascent.readthedocs.io/.
//
// Please also read ascent/LICENSE
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the disclaimer below.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the disclaimer (as noted below) in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of the LLNS/LLNL nor the names of its contributors may
//   be used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL LAWRENCE LIVERMORE NATIONAL SECURITY,
// LLC, THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
// IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

//-----------------------------------------------------------------------------
///
/// file: ascent_derived_jit.cpp
///
//-----------------------------------------------------------------------------

#include "ascent_derived_jit.hpp"
#include "ascent_blueprint_architect.hpp"
#include "ascent_expressions_ast.hpp"

#include <ascent_logging.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <occa.hpp>

//-----------------------------------------------------------------------------
// -- begin ascent:: --
//-----------------------------------------------------------------------------
namespace ascent
{

//-----------------------------------------------------------------------------
// -- begin ascent::runtime --
//-----------------------------------------------------------------------------
namespace runtime
{

//-----------------------------------------------------------------------------
// -- begin ascent::runtime::expressions--
//-----------------------------------------------------------------------------
namespace expressions
{

namespace detail
{

void
pack_mesh(const conduit::Node &dom,
          const int invoke_size,
          const std::string &topo_name,
          occa::device &device,
          occa::kernel &kernel,
          std::vector<occa::memory> &mem)
{
  const conduit::Node &n_topo = dom["topologies/" + topo_name];
  const std::string coords_name = n_topo["coordset"].as_string();
  const std::string topo_type = n_topo["type"].as_string();
  const conduit::Node &n_coords = dom["coordsets/" + coords_name];

  if(topo_type == "uniform")
  {
    int dim_x = n_coords["dims/i"].to_int32();
    int dim_y = n_coords["dims/j"].to_int32();

    kernel.pushArg(dim_x);
    kernel.pushArg(dim_y);

    if(n_coords.has_path("dims/k"))
    {
      int dim_z = n_coords["dims/k"].to_int32();
      kernel.pushArg(dim_z);
    }

    const conduit::Node &n_spacing = n_coords["spacing"];
    double dx = n_spacing["dx"].to_float64();
    double dy = n_spacing["dy"].to_float64();
    kernel.pushArg(dx);
    kernel.pushArg(dy);

    if(n_spacing.has_path("dz"))
    {
      double dz = n_spacing["dz"].to_float64();
      kernel.pushArg(dz);
    }

    const conduit::Node &n_origin = n_coords["origin"];

    double ox = n_origin["x"].to_float64();
    double oy = n_origin["y"].to_float64();
    kernel.pushArg(ox);
    kernel.pushArg(oy);

    if(n_origin.has_child("z"))
    {
      double oz = n_origin["z"].to_float64();
      kernel.pushArg(oz);
    }
  }
  else if(topo_type == "rectilinear")
  {
    int dim_x = n_coords["values/x"].dtype().number_of_elements();
    int dim_y = n_coords["values/y"].dtype().number_of_elements();
    int dim_z = 0;
    kernel.pushArg(dim_x);
    kernel.pushArg(dim_y);

    if(n_coords.has_path("values/z"))
    {
      dim_z = n_coords["values/z"].dtype().number_of_elements();
      kernel.pushArg(dim_z);
    }

    bool is_double = n_coords["values/x"].dtype().is_float64();
    if(is_double)
    {
      occa::memory ox_vals;
      occa::memory oy_vals;
      // zero copy occa::wrapMemory(???
      const double *x_vals = n_coords["values/x"].as_float64_ptr();
      ox_vals = device.malloc(dim_x * sizeof(double), x_vals);
      const double *y_vals = n_coords["values/y"].as_float64_ptr();
      oy_vals = device.malloc(dim_y * sizeof(double), y_vals);

      kernel.pushArg(ox_vals);
      kernel.pushArg(oy_vals);
      mem.push_back(ox_vals);
      mem.push_back(oy_vals);

      if(n_coords.has_path("values/z"))
      {
        occa::memory oz_vals;
        const double *z_vals = n_coords["values/z"].as_float64_ptr();
        oz_vals = device.malloc(dim_z * sizeof(double), z_vals);
        kernel.pushArg(oz_vals);
        mem.push_back(oz_vals);
      }
    }

    // conduit::Node n_tmp;
    // n_tmp.set_external(DataType::float64(num_vals),ptr);
    // n_vals.to_float64_array(n_tmp);
  }
}

void
pack_field(const conduit::Node &field,
           occa::device &device,
           occa::memory &field_memory,
           occa::kernel &kernel)
{
  const int size = field["values"].dtype().number_of_elements();
  // zero copy occa::wrapMemory(???
  if(field["values"].dtype().is_float64())
  {
    const double *vals = field["values"].as_float64_ptr();
    field_memory = device.malloc(size * sizeof(double), vals);
  }
  else
  {
    const float *vals = field["values"].as_float32_ptr();
    field_memory = device.malloc(size * sizeof(float), vals);
  }
  std::cout << "Mode " << field_memory.mode() << "\n";
  kernel.pushArg(field_memory);
}

void
pack_fields(const conduit::Node &dom,
            const std::map<std::string, std::string> &var_types,
            const int invoke_size,
            occa::device &device,
            std::vector<occa::memory> &field_memory)
{
  // extract the field args
  for(auto vt : var_types)
  {
    const std::string &var_name = vt.first;
    const std::string &type = vt.second;
    const conduit::Node &field = dom["fields/" + var_name];
    const int size = field["values"].dtype().number_of_elements();
    if(invoke_size != size)
    {
      ASCENT_ERROR("field sizes do not match " << invoke_size << " " << size);
    }
    occa::memory o_vals;
    // we only have two types currently
    // zero copy occa::wrapMemory(???
    if(type == "double")
    {
      const double *vals = field["values"].as_float64_ptr();
      o_vals = device.malloc(size * sizeof(double), vals);
    }
    else
    {
      const float *vals = field["values"].as_float32_ptr();
      o_vals = device.malloc(size * sizeof(float), vals);
    }
    std::cout << "Mode " << o_vals.mode() << "\n";

    field_memory.push_back(o_vals);
  }
}
// void override_print(const char *str)
//{
//  std::cout<<"HERE "<<str<<"\n";
//  ASCENT_ERROR("OCCA error" <<str);
//}

void
modes()
{
  auto modes = occa::modeMap();
  for(auto mode : modes)
  {
    std::cout << "mode " << mode.first << "\n";
  }
  // occa::io::stderr.setOverride(&override_print);
}

// TODO prefix topo_name to the parameters
std::string
topo_params(const std::string &topo_name, const conduit::Node &dom)

{
  const std::string topo_type =
      dom["topologies/" + topo_name + "/type"].as_string();
  const int dims = topo_dim(topo_name, dom);
  std::stringstream ss;

  if(topo_type == "uniform")
  {
    constexpr char uni3d[] = "                 const int point_dims_x,\n"
                             "                 const int point_dims_y,\n"
                             "                 const int point_dims_z,\n"
                             "                 const double spacing_x,\n"
                             "                 const double spacing_y,\n"
                             "                 const double spacing_z,\n"
                             "                 const double origin_x,\n"
                             "                 const double origin_y,\n"
                             "                 const double origin_z,\n";

    constexpr char uni2d[] = "                 const int point_dims_x,\n"
                             "                 const int point_dims_y,\n"
                             "                 const double spacing_x,\n"
                             "                 const double spacing_y,\n"
                             "                 const double origin_x,\n"
                             "                 const double origin_y,\n";
    if(dims == 3)
    {
      ss << uni3d;
    }
    else
    {
      ss << uni2d;
    }
  }
  else if(topo_type == "rectilinear" || topo_type == "structured")
  {
    constexpr char structured3d[] =
        "                 const int point_dims_x,\n"
        "                 const int point_dims_y,\n"
        "                 const int point_dims_z,\n"
        "                 const double * coords_x,\n"
        "                 const double * coords_y,\n"
        "                 const double * coords_z,\n";
    constexpr char structured2d[] =
        "                 const int point_dims_x,\n"
        "                 const int point_dims_y,\n"
        "                 const double * coords_x,\n"
        "                 const double * coords_y,\n";
    if(dims == 3)
    {
      ss << structured3d;
    }
    else
    {
      ss << structured2d;
    }
  }
  else if(topo_type == "unstructured")
  {
    constexpr char unstructured3d[] =
        "                 const int size_x,\n"
        "                 const int size_y,\n"
        "                 const int size_z,\n"
        "                 const double * coords_x,\n"
        "                 const double * coords_y,\n"
        "                 const double * coords_z,\n"
        "                 const int * cell_conn,\n"
        "                 const int cell_shape,\n";
    constexpr char unstructured2d[] =
        "                 const int size_x,\n"
        "                 const int size_y,\n"
        "                 const double * coords_x,\n"
        "                 const double * coords_y,\n"
        "                 const int * cell_conn,\n"
        "                 const int cell_shape,\n";
    if(dims == 3)
    {
      ss << unstructured3d;
    }
    else
    {
      ss << unstructured2d;
    }
  }
  return ss.str();
}

void
mesh_params(const conduit::Node &mesh_meta, std::stringstream &ss)

{
  const std::string mesh_type = mesh_meta["type"].as_string();
  const int dims = mesh_meta["spatial_dims"].to_int32();
  const std::string data_type = mesh_meta["data_type"].as_string();

  if(mesh_type == "uniform")
  {
    constexpr char uni3d[] = "                 const int point_dims_x,\n"
                             "                 const int point_dims_y,\n"
                             "                 const int point_dims_z,\n"
                             "                 const double spacing_x,\n"
                             "                 const double spacing_y,\n"
                             "                 const double spacing_z,\n"
                             "                 const double origin_x,\n"
                             "                 const double origin_y,\n"
                             "                 const double origin_z,\n";

    constexpr char uni2d[] = "                 const int point_dims_x,\n"
                             "                 const int point_dims_y,\n"
                             "                 const double spacing_x,\n"
                             "                 const double spacing_y,\n"
                             "                 const double origin_x,\n"
                             "                 const double origin_y,\n";
    if(dims == 3)
    {
      ss << uni3d;
    }
    else
    {
      ss << uni2d;
    }
  }
  else if(mesh_type == "rectilinear" || mesh_type == "structured")
  {
    if(data_type == "double")
    {
      constexpr char structured3d[] =
          "                 const int point_dims_x,\n"
          "                 const int point_dims_y,\n"
          "                 const int point_dims_z,\n"
          "                 const double * coords_x,\n"
          "                 const double * coords_y,\n"
          "                 const double * coords_z,\n";
      constexpr char structured2d[] =
          "                 const int point_dims_x,\n"
          "                 const int point_dims_y,\n"
          "                 const double * coords_x,\n"
          "                 const double * coords_y,\n";
      if(dims == 3)
      {
        ss << structured3d;
      }
      else
      {
        ss << structured2d;
      }
    }
    else
    {
      constexpr char structured3d[] =
          "                 const int point_dims_x,\n"
          "                 const int point_dims_y,\n"
          "                 const int point_dims_z,\n"
          "                 const float * coords_x,\n"
          "                 const float * coords_y,\n"
          "                 const float * coords_z,\n";
      constexpr char structured2d[] =
          "                 const int point_dims_x,\n"
          "                 const int point_dims_y,\n"
          "                 const float * coords_x,\n"
          "                 const float * coords_y,\n";
      if(dims == 3)
      {
        ss << structured3d;
      }
      else
      {
        ss << structured2d;
      }
    }
  }
  else if(mesh_type == "unstructured")
  {
    if(data_type == "double")
    {
      constexpr char unstructured3d[] =
          "                 const int size_x,\n"
          "                 const int size_y,\n"
          "                 const int size_z,\n"
          "                 const double * coords_x,\n"
          "                 const double * coords_y,\n"
          "                 const double * coords_z,\n"
          "                 const int * cell_conn,\n"
          "                 const int cell_shape,\n";
      constexpr char unstructured2d[] =
          "                 const int size_x,\n"
          "                 const int size_y,\n"
          "                 const double * coords_x,\n"
          "                 const double * coords_y,\n"
          "                 const int * cell_conn,\n"
          "                 const int cell_shape,\n";
      if(dims == 3)
      {
        ss << unstructured3d;
      }
      else
      {
        ss << unstructured2d;
      }
    }
    else
    {
      if(data_type == "double")
      {
        constexpr char unstructured3d[] =
            "                 const int size_x,\n"
            "                 const int size_y,\n"
            "                 const int size_z,\n"
            "                 const double * coords_x,\n"
            "                 const double * coords_y,\n"
            "                 const double * coords_z,\n"
            "                 const int * cell_conn,\n"
            "                 const int cell_shape,\n";
        constexpr char unstructured2d[] =
            "                 const int size_x,\n"
            "                 const int size_y,\n"
            "                 const double * coords_x,\n"
            "                 const double * coords_y,\n"
            "                 const int * cell_conn,\n"
            "                 const int cell_shape,\n";
        if(dims == 3)
        {
          ss << unstructured3d;
        }
        else
        {
          ss << unstructured2d;
        }
      }
      else
      {
        constexpr char unstructured3d[] =
            "                 const int size_x,\n"
            "                 const int size_y,\n"
            "                 const int size_z,\n"
            "                 const float * coords_x,\n"
            "                 const float * coords_y,\n"
            "                 const float * coords_z,\n"
            "                 const int * cell_conn,\n"
            "                 const int cell_shape,\n";
        constexpr char unstructured2d[] =
            "                 const int size_x,\n"
            "                 const int size_y,\n"
            "                 const float * coords_x,\n"
            "                 const float * coords_y,\n"
            "                 const int * cell_conn,\n"
            "                 const int cell_shape,\n";
        if(dims == 3)
        {
          ss << unstructured3d;
        }
        else
        {
          ss << unstructured2d;
        }
      }
    }
  }
}

void
mesh_function(const std::string &func,
              const conduit::Node &mesh_meta,
              std::stringstream &ss)
{
  const std::string mesh_type = mesh_meta["type"].as_string();
  const int dims = mesh_meta["spatial_dims"].to_int32();
  // the id of the cell is the variable 'n'

  if(func == "volume")
  {
    if(mesh_type == "uniform")
    {
      ss << "        double volume;\n";
      ss << "        volume = spacing_x * spacing_y * spacing_z;\n";
    }
    else if(mesh_type == "rectilinear")
    {
      mesh_function("cell_idx", mesh_meta, ss);
      ss << "        double volume;\n";
      // ss <<"printf(\"idx %d %d %d\\n\", cell_idx[0], cell_idx[1],
      // cell_idx[2]);\n";
      ss << "        double dx = coords_x[cell_idx[0]+1] - "
            "coords_x[cell_idx[0]];\n";
      ss << "        double dy = coords_y[cell_idx[1]+1] - "
            "coords_y[cell_idx[1]];\n";
      ss << "        double dz = coords_z[cell_idx[2]+1] - "
            "coords_z[cell_idx[2]];\n";
      ss << "        volume = dx * dy * dz;\n";
    }
    else
    {
      ASCENT_ERROR("not implemented");
    }
  }
  else if(func == "cell_idx")
  {
    if(dims == 2)
    {
      ss << "        int cell_idx[2];\n";
      ss << "        cell_idx[0] = n \% (point_dims_x - 1);\n";
      ss << "        cell_idx[1] = n / (point_dims_x - 1);\n";
    }
    else
    {
      ss << "        int cell_idx[3];\n";
      ss << "        cell_idx[0] = n % (point_dims_x - 1);\n";
      ss << "        cell_idx[1] = (n / (point_dims_x - 1)) % (point_dims_y - "
            "1);\n";
      ss << "        cell_idx[2] = n / ((point_dims_x - 1) * (point_dims_y - "
            "1));\n";
    }
  }
  else
  {
    ASCENT_ERROR("Mesh function not implemented '" << func << "'");
  }
}

std::string
mesh_string(int type)
{
  std::string res;
  if(type == 0)
    res = "points";
  if(type == 1)
    res = "uniform";
  if(type == 2)
    res = "rectilinear";
  if(type == 3)
    res = "structured";
  if(type == 4)
    res = "unstructured";
  return res;
}

std::string
check_meshes(const conduit::Node &dataset, const conduit::Node &vars)
{
  bool valid = true;
  std::vector<std::string> bad_names;
  const int num_names = vars.number_of_children();
  if(num_names > 1)
  {
    ASCENT_ERROR("Expressions only support a single topology");
  }

  const std::string topo = vars.child(0).as_string();

  if(!has_topology(dataset, topo))
  {
    bad_names.push_back(topo);
    valid = false;
  }

  if(!valid)
  {
    std::stringstream bad_list;
    bad_list << "[";
    for(auto bad : bad_names)
    {
      bad_list << bad << " ";
    }
    bad_list << "]";

    std::vector<std::string> names =
        dataset.child(0)["topologies"].child_names();
    std::stringstream ss;
    ss << "[";
    for(int i = 0; i < names.size(); ++i)
    {
      ss << " " << names[i];
    }
    ss << "]";
    ASCENT_ERROR("Field: dataset does not contain topologies '"
                 << bad_list.str() << "'"
                 << " known = " << ss.str());
  }

  // points, uniform, rectilinear, curvilinear, unstructured
  int mesh_types[5] = {0, 0, 0, 0, 0};
  topology_types(dataset, topo, mesh_types);

  // to make things less complicated we need to enforce that everyone has the
  // same mesh type
  int type_count = 0;
  int type_index = -1;
  for(int i = 0; i < 5; ++i)
  {
    if(mesh_types[i] > 0)
    {
      type_count += 1;
      type_index = i;
      std::cout << "meshes " << i << " count " << mesh_types[i] << "\n";
    }
  }
  if(type_count > 1)
  {
    ASCENT_ERROR("Currenly only support the same mesh type");
  }
  return mesh_string(type_index);
}

std::string
check_fields(const conduit::Node &dataset, const conduit::Node &vars)
{
  bool valid = true;
  std::vector<std::string> bad_names;
  for(int i = 0; i < vars.number_of_children(); ++i)
  {
    std::string field = vars.child(i).as_string();
    if(!has_field(dataset, field))
    {
      bad_names.push_back(field);
      valid = false;
    }
  }

  if(!valid)
  {
    std::stringstream bad_list;
    bad_list << "[";
    for(auto bad : bad_names)
    {
      bad_list << bad << " ";
    }
    bad_list << "]";

    std::vector<std::string> names = dataset.child(0)["fields"].child_names();
    std::stringstream ss;
    ss << "[";
    for(int i = 0; i < names.size(); ++i)
    {
      ss << " " << names[i];
    }
    ss << "]";
    ASCENT_ERROR("Field: dataset does not contain fields '"
                 << bad_list.str() << "'"
                 << " known = " << ss.str());
  }

  // we have valid fields. Now check for valid assocs
  std::set<std::string> assocs_set;
  std::map<std::string, std::string> assocs_map;
  // we have the same number of vars on each rank so
  // mpi comm is safe (ie. same expression everywhere)
  for(int i = 0; i < vars.number_of_children(); ++i)
  {
    std::string field = vars.child(i).as_string();
    std::string assoc = field_assoc(dataset, field);
    assocs_set.insert(assoc);
    assocs_map[field] = assoc;
  }

  if(assocs_set.size() > 1)
  {
    std::stringstream ss;
    for(auto assoc : assocs_map)
    {
      ss << assoc.first << " : " << assoc.second << "\n";
    }
    ASCENT_ERROR("Error: expression has fields of mixed assocation."
                 << " They all must be either 'element' or 'vertex'\n"
                 << ss.str());
  }
  return *assocs_set.begin();
}

std::string
create_map_kernel(std::map<std::string, std::string> &in_vars,
                  std::map<std::string, double> &in_constants,
                  const conduit::Node &mesh_meta,
                  std::string out_type,
                  std::string expr)
{
  std::set<std::string> mesh_functions;
  if(mesh_meta.has_path("mesh_functions"))
  {
    for(int i = 0; i < mesh_meta["mesh_functions"].number_of_children(); ++i)
    {
      mesh_functions.insert(mesh_meta["mesh_functions"].child(i).as_string());
    }
  }

  std::stringstream ss;
  ss << "@kernel void map(const int entries,\n";
  // add in all field arrays
  for(auto var : in_vars)
  {
    ss << "                 const " << var.second << " *" << var.first
       << "_ptr,\n";
  }
  // add in all pre-executed constants
  for(auto c : in_constants)
  {
    ss << "                 const double " << c.first << ",\n";
  }

  if(mesh_meta.number_of_children() != 0)
  {
    detail::mesh_params(mesh_meta, ss);
  }

  ss << "                 " << out_type << " *output_ptr)\n"
     << "{\n"
     << "  for (int group = 0; group < entries; group += 128; @outer)\n"
     << "  {\n"
     << "    for (int item = group; item < (group + 128); ++item; @inner)\n"
     << "    {\n"
     << "      const int n = item;\n\n"
     << "      if (n < entries)\n"
     << "      {\n";
  for(auto var : in_vars)
  {
    ss << "        const " << var.second << " " << var.first << " = "
       << var.first << "_ptr[n];\n";
  }

  for(auto func : mesh_functions)
  {
    mesh_function(func, mesh_meta, ss);
  }
  ss << "        " << out_type << " output;\n"
     << "        output = " << expr
     << ";\n"
     // << "        printf(\" spacing %f \",spacing_x);\n"
     << "        output_ptr[n] = output;\n"
     << "      }\n"
     << "    }\n"
     << "  }\n"
     << '}';
  return ss.str();
}

}; // namespace detail

void
do_it(conduit::Node &dataset, std::string expr, const conduit::Node &info)
{
  std::cout << "doint it\n";
  info.print();
  const int num_domains = dataset.number_of_children();
  std::cout << "Domains " << num_domains << "\n";

  // Validation: make sure fields/topo are there
  // and have compatible associations
  conduit::Node meta;
  validate(dataset, info, meta);

  // set up the field and constant kernel params
  std::map<std::string, std::string> var_types; // name:type
  std::map<std::string, double> constants;
  parameters(dataset, info, var_types, constants);

  std::string kernel_str = detail::create_map_kernel(var_types,
                                                     constants,
                                                     meta["mesh"],
                                                     "double", // output type
                                                     expr);

  std::cout << kernel_str << "\n";

  detail::modes();
  occa::setDevice("mode: 'OpenCL', platform_id: 0, device_id: 1");
  occa::device &device = occa::getDevice();
  occa::kernel kernel;

  try
  {
    kernel = device.buildKernelFromString(kernel_str, "map");
  }
  catch(const occa::exception &e)
  {
    ASCENT_ERROR("Expression compilation failed:\n" << e.what());
  }
  catch(...)
  {
    ASCENT_ERROR("Expression compilation failed with an unknown error");
  }

  const std::string assoc = meta["assoc"].as_string();
  const std::string topology = meta["topology"].as_string();

  if(topology == "")
  {
    ASCENT_ERROR("OMG");
  }

  for(int i = 0; i < num_domains; ++i)
  {
    // By the time we get here, all validation has taken
    // place, so its not possible that anything is wrong.....

    // TODO: we need to skip domains that don't have what we need

    conduit::Node &dom = dataset.child(i);
    if(!dom.has_path("topologies/" + topology))
    {
      continue;
    }
    kernel.clearArgs();

    int invoke_size;
    if(assoc == "element")
    {
      invoke_size = num_cells(dom, topology);
    }
    else
    {
      invoke_size = num_points(dom, topology);
    }

    // these are reference counted
    // need to keep the mem in scope or bad things happen
    std::vector<occa::memory> field_memory;
    if(info.has_path("field_vars"))
    {
      detail::pack_fields(dom, var_types, invoke_size, device, field_memory);
    }

    // pass invocation size
    kernel.pushArg(invoke_size);
    // pass the field arrays
    for(auto mem : field_memory)
    {
      kernel.pushArg(mem);
    }

    // pass in the constants
    for(auto cnst : constants)
    {
      if(cnst.first == "domain_id")
      {
        double dom_id = dom["state/domain_id"].to_double();
        kernel.pushArg(dom_id);
      }
      else
      {
        kernel.pushArg(cnst.second);
      }
    }
    // need to keep the mem in scope or bad things happen
    std::vector<occa::memory> mesh_memory;
    if(info.has_path("mesh_vars"))
    {
      detail::pack_mesh(
          dom, invoke_size, topology, device, kernel, mesh_memory);
    }

    std::cout << "INVOKE SIZE " << invoke_size << "\n";
    conduit::Node &n_output = dom["fields/output"];
    n_output["association"] = assoc;
    n_output["topology"] = topology;

    n_output["values"] = conduit::DataType::float64(invoke_size);
    double *output_ptr = n_output["values"].as_float64_ptr();
    occa::array<double> o_output(invoke_size);

    kernel.pushArg(o_output);
    kernel.run();

    o_output.memory().copyTo(output_ptr);

    dom["fields/output"].print();
  }
}

void
validate(const conduit::Node &dataset,
         const conduit::Node &info,
         conduit::Node &meta)
{
  std::string assoc;
  std::string topo_name;

  bool has_variable = false;
  if(info.has_path("field_vars"))
  {
    has_variable = true;
    assoc = detail::check_fields(dataset, info["field_vars"]);
  }

  std::set<std::string> mesh_functions;
  if(info.has_path("mesh_vars"))
  {
    has_variable = true;
    if(assoc == "vertex")
    {
      ASCENT_ERROR("Mixed vertex field with mesh variable. All "
                   << "variables must be element centered");
    }
    else
    {
      assoc = "element";
    }

    std::string mesh_type = detail::check_meshes(dataset, info["mesh_vars"]);
    // we only allow one topology
    topo_name = info["mesh_vars"].child(0).as_string();
    meta["mesh/topology"] = topo_name;
    meta["mesh/type"] = mesh_type;
    meta["mesh/spatial_dims"] = spatial_dims(dataset, topo_name);
    meta["mesh/data_type"] = coord_type(dataset, topo_name);

    if(info.has_path("mesh_functions"))
    {
      meta["mesh/mesh_functions"] = info["mesh_functions"];
      for(int i = 0; i < info["mesh_functions"].number_of_children(); ++i)
      {
        mesh_functions.insert(info["mesh_functions"].child(i).as_string());
      }
    }
  }

  if(!has_variable)
  {
    ASCENT_ERROR(
        "There has to be at least one mesh/variable in the expression");
  }

  meta["assoc"] = assoc;
  // we need the topology so we can
  //    1) check to see if we execute on a domain
  //    2) to determine the kernel lauch size
  if(topo_name == "")
  {
    // if topo name isn't set, then we have at least one field
    const std::string field = info["field_vars"].child(0).as_string();
    topo_name = field_topology(dataset, field);
  }
  meta["topology"] = topo_name;
}

void
parameters(const conduit::Node &dataset,
           const conduit::Node &info,
           std::map<std::string, std::string> &var_types, // name:type
           std::map<std::string, double> &constants)
{

  std::set<std::string> var_names;

  if(info.has_path("field_vars"))
  {
    for(int i = 0; i < info["field_vars"].number_of_children(); ++i)
    {
      var_names.insert(info["field_vars"].child(i).as_string());
    }
  }

  for(auto name : var_names)
  {
    std::string type = field_type(dataset, name);
    var_types[name] = type;
  }

  // indentify constants. we will treat them all as
  // doubles
  if(info.has_path("constants"))
  {
    for(int i = 0; i < info["constants"].number_of_children(); ++i)
    {
      const conduit::Node &constant = info["constants"].child(i);
      std::string name = constant["name"].as_string();
      double value = -1.;
      if(constant.has_path("value"))
      {
        value = constant["value"].to_float64();
      }
      else if(name != "domain_id")
      {
        ASCENT_ERROR("only domain id can be missing a value " << name);
      }
      constants[name] = value;
    }
  }
}

//-----------------------------------------------------------------------------
// clang-format off
std::string
generate_loop(const conduit::Node &kernel, const std::string& output)
{
  std::stringstream ss;

  ss << "  for (int group = 0; group < entries; group += 128; @outer)\n"
     << "  {\n"
     << "    for (int item = group; item < (group + 128); ++item; @inner)\n"
     << "    {\n"
     << "      if (item < entries)\n"
     << "      {\n"
     <<          kernel["for_body"].as_string()
     << "        double output = " << kernel["expr"].as_string() << ";\n"
     << "        " << output << "_ptr[item] = output;\n"
     << "      }\n"
     << "    }\n"
     << "  }\n";
  return ss.str();
}

std::string
generate_kernel(const conduit::Node &kernel)
{
  std::stringstream ss;
  ss << "@kernel void map(const int entries,\n"
     << kernel["params"].as_string()
     << "                 double *output_ptr)\n"
     << "{\n"
     << kernel["kernel_body"].as_string()
     << "}";
  return ss.str();
}

// clang-format on

std::string
remove_duplicate_lines(const std::string &input_str)
{
  std::stringstream ss(input_str);
  std::string line;
  std::unordered_set<std::string> lines;
  std::string output_str;
  while(std::getline(ss, line))
  {
    if(lines.find(line) == lines.end())
    {
      output_str += line + "\n";
      lines.insert(line);
    }
  }
  return output_str;
}

void
remove_duplicate_params(conduit::Node &kernel)
{
  std::stringstream ss(kernel["params"].as_string());
  std::unordered_set<std::string> lines;
  std::string line;
  std::string params;
  for(int i = 0; std::getline(ss, line); ++i)
  {
    if(lines.find(line) == lines.end())
    {
      params += line + "\n";
      lines.insert(line);
    }
    else
    {
      kernel["args"].remove(i--);
    }
  }
  kernel["params"] = params;
}

//-----------------------------------------------------------------------------
// TODO for now we just put the field on the mesh when calling execute
// should probably delete it later if it's an intermediate field
void
execute_jitable(conduit::Node &jitable, conduit::Node &dataset)
{
  // TODO set this automatically?
  occa::setDevice("mode: 'OpenCL', platform_id: 0, device_id: 1");
  occa::device &device = occa::getDevice();
  occa::kernel occa_kernel;

  // we need an association and topo so we can put the field back on the mesh
  // TODO create a new topo with vertex assoc for temporary fields
  if(jitable["topology"].as_string().empty())
  {
    ASCENT_ERROR("Error while executing derived field: Could not determine the "
                 "topology.");
  }
  if(jitable["association"].as_string().empty())
  {
    ASCENT_ERROR("Error while executing derived field: Could not determine the "
                 "association.");
  }

  // make sure all the code is moved to kernel_body
  const int num_kernels = jitable["kernels"].number_of_children();
  for(int i = 0; i < num_kernels; ++i)
  {
    conduit::Node &kernel = jitable["kernels"].child(i);
    if(!kernel["expr"].as_string().empty())
    {
      kernel["kernel_body"] =
          kernel["kernel_body"].as_string() + generate_loop(kernel, "output");
      kernel["expr"] = "";
      kernel["for_body"] = "";
    }
    // run fixes and optimizations
    remove_duplicate_params(kernel);
    kernel["kernel_body"] =
        remove_duplicate_lines(kernel["kernel_body"].as_string());
  }

  const int num_domains = dataset.number_of_children();
  for(int i = 0; i < num_domains; ++i)
  {
    conduit::Node &dom = dataset.child(i);

    const std::string &kernel_type =
        jitable["dom_info"].child(i)["kernel_type"].as_string();
    conduit::Node &kernel = jitable["kernels/" + kernel_type];

    const std::string kernel_string = generate_kernel(kernel);

    const int entries = jitable["dom_info"].child(i)["entries"].as_int32();

    std::cout << kernel_string << std::endl;

    try
    {
      occa_kernel = device.buildKernelFromString(kernel_string, "map");
    }
    catch(const occa::exception &e)
    {
      ASCENT_ERROR("Jitable: Expression compilation failed:\n"
                   << e.what() << "\n\n"
                   << kernel_string);
    }
    catch(...)
    {
      ASCENT_ERROR(
          "Jitable: Expression compilation failed with an unknown error");
    }

    occa_kernel.clearArgs();

    // pass invocation size
    occa_kernel.pushArg(entries);

    // these are reference counted
    // need to keep the mem in scope or bad things happen
    std::vector<occa::memory> field_memories;
    const int num_args = kernel["args"].number_of_children();
    for(int i = 0; i < num_args; ++i)
    {
      const conduit::Node &arg = kernel["args"].child(i);
      const std::string arg_type = arg["type"].as_string();
      // TODO we need to create utils file for things like is_scalar
      if(arg_type == "int" || arg_type == "double")
      {
        occa_kernel.pushArg(arg["value"].to_float64());
      }
      else if(arg_type == "field")
      {
        field_memories.emplace_back();
        const conduit::Node &field = dom["fields/" + arg["value"].as_string()];
        detail::pack_field(field, device, field_memories.back(), occa_kernel);
      }
      else if(arg_type == "topo")
      {
        // detail::pack_topo(
        //     arg["value"].as_string(), dom, device, field_memory);
      }
      else
      {
        ASCENT_ERROR("Unknown JIT kernel argument type: '" << arg_type << "'");
      }
    }

    std::cout << "INVOKE SIZE " << entries << "\n";
    conduit::Node &n_output = dom["fields/output"];
    n_output["association"] = jitable["association"];
    n_output["topology"] = jitable["topology"];

    n_output["values"] = conduit::DataType::float64(entries);
    double *output_ptr = n_output["values"].as_float64_ptr();
    occa::array<double> o_output(entries);

    occa_kernel.pushArg(o_output);
    occa_kernel.run();

    o_output.memory().copyTo(output_ptr);

    dom["fields/output"].print();
  }
}
//-----------------------------------------------------------------------------
};
//-----------------------------------------------------------------------------
// -- end ascent::runtime::expressions--
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
};
//-----------------------------------------------------------------------------
// -- end ascent::runtime --
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
};
//-----------------------------------------------------------------------------
// -- end ascent:: --
//-----------------------------------------------------------------------------
