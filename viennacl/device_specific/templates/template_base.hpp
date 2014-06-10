#ifndef VIENNACL_DEVICE_SPECIFIC_TEMPLATES_TEMPLATE_BASE_BASE
#define VIENNACL_DEVICE_SPECIFIC_TEMPLATES_TEMPLATE_BASE_BASE

/* =========================================================================
   Copyright (c) 2010-2013, Institute for Microelectronics,
                            Institute for Analysis and Scientific Computing,
                            TU Wien.
   Portions of this software are copyright by UChicago Argonne, LLC.

                            -----------------
                  ViennaCL - The Vienna Computing Library
                            -----------------

   Project Head:    Karl Rupp                   rupp@iue.tuwien.ac.at

   (A list of authors and contributors can be found in the PDF manual)

   License:         MIT (X11), see file LICENSE in the base directory
============================================================================= */


/** @file viennacl/generator/profile_base.hpp
 *
 * Base classes for the profiles
*/

#include <list>
#include <set>

#include "viennacl/ocl/kernel.hpp"
#include "viennacl/ocl/device.hpp"
#include "viennacl/ocl/device_utils.hpp"
#include "viennacl/ocl/infos.hpp"

#include "viennacl/scheduler/forwards.h"

#include "viennacl/device_specific/tree_parsing/traverse.hpp"
#include "viennacl/device_specific/tree_parsing/map.hpp"
#include "viennacl/device_specific/tree_parsing/prototype_generation.hpp"
#include "viennacl/device_specific/tree_parsing/set_arguments.hpp"

namespace viennacl{

  namespace device_specific{


    /** @brief Base class for an operation profile */
    class template_base{
      protected:
        virtual bool invalid_impl(viennacl::ocl::device const & /*dev*/, size_t /*scalartype_size*/) const { return false; }

        virtual unsigned int lmem_used(unsigned int /*scalartype_size*/) const { return 0; }

        /** @brief Generates the body of the associated kernel function
         *
         *  @param kernel_id  If this profile requires multiple kernel, the index for which the core should be generated
         *  @param stream     The output stream the kernel is written to
         *  @param statements the statements for which the code should be generated
         *  @param mapping    the mapping of the statement_nodes to the mapped_objects
         */
        virtual void core(unsigned int kernel_id, utils::kernel_generation_stream& stream, statements_container const & statements, std::vector<mapping_type> const & mapping) const = 0;

        /** @brief Configures the range and enqueues the arguments associated with the profile
         *
         * @param kernel_id If this profile requires multiple kernel, the index for which the core should be generated
         *
         * @param kernel the kernel object
         * @param n_arg a dummy reference for keeping track of the added arguments
         */
        virtual void configure_impl(unsigned int kernel_id, statements_container const & statements, viennacl::ocl::kernel & kernel, unsigned int & n_arg)  const = 0;

        virtual void add_kernel_arguments(statements_container const & statements, std::string & arguments_string) const = 0;

        virtual void init_simd_width(mapping_type::value_type const & v) const
        {
          if(mapped_handle * p = dynamic_cast<mapped_handle *>(v.second.get()))
            p->set_simd_width(simd_width_);
        }



      public:
        /** @brief The constructor */
        template_base(const char * scalartype, unsigned int simd_width, unsigned int local_size_1, unsigned int local_size_2, unsigned int num_kernels) :
          scalartype_(scalartype), simd_width_(simd_width), local_size_0_(local_size_1), local_size_1_(local_size_2), num_kernels_(num_kernels)
        {

        }

        /** @brief The destructor */
        virtual ~template_base()
        {

        }

        /** @brief Returns the number of kernels used by the template */
        unsigned int num_kernels() const
        {
          return num_kernels_;
        }


        /** @brief returns whether or not the profile leads to undefined behavior on particular device
         *  @param dev               the given device
         *  @param scalartype_size   Local memory required to execute the kernel
         */
        bool is_invalid() const
        {
          bool invalid = false;
          viennacl::ocl::device const & dev = viennacl::ocl::current_device();

          //Query device informations
          size_t lmem_available = static_cast<size_t>(dev.local_mem_size());
          unsigned int scalartype_size = utils::scalartype_size(scalartype_);
          invalid |= (lmem_used(scalartype_size)>lmem_available);

          //Invalid work group size
          size_t max_workgroup_size = dev.max_work_group_size();
          std::vector<size_t> max_work_item_sizes = dev.max_work_item_sizes();
          invalid |= local_size_0_*local_size_1_ > max_workgroup_size
              || local_size_0_ > max_work_item_sizes[0]
              || local_size_1_ > max_work_item_sizes[1]; // uses too much resources
		  

          //Not warp multiple
          if(dev.type()==CL_DEVICE_TYPE_GPU){
            unsigned int warp_size = 32;
            if(dev.vendor_id()==4098)
              warp_size = 64;
            invalid |= (((local_size_0_*local_size_1_)%warp_size)>0);
          }

          //Invalid SIMD Width
          invalid |= (simd_width_!=1 && simd_width_!=2 &&
                      simd_width_!=4 && simd_width_!=8 &&
                      simd_width_!=16);

          return  invalid || invalid_impl(dev, scalartype_size);
        }

        void configure(statements_container const & statements, std::vector<viennacl::ocl::kernel*> & kernels, binding_policy_t binding_policy)
        {
          //Configure
          for(std::vector<viennacl::ocl::kernel*>::iterator it = kernels.begin() ; it != kernels.end() ; ++it)
          {
            unsigned int current_arg = 0;
            tools::shared_ptr<symbolic_binder> binder = make_binder(binding_policy);
            (*it)->local_work_size(0,local_size_0_);
            (*it)->local_work_size(1,local_size_1_);
            configure_impl(std::distance(kernels.begin(), it), statements, **it, current_arg);
            for(typename statements_container::data_type::const_iterator itt = statements.data().begin() ; itt != statements.data().end() ; ++itt)
              tree_parsing::traverse(*itt, itt->root(), tree_parsing::set_arguments_functor(*binder,current_arg,**it));
          }
        }

        /** @brief Generates the code associated with this profile onto the provided stream
         *  Redirects to the virtual core() method
         */
        std::string generate(statements_container const & statements, std::vector<mapping_type>  const & mapping, std::string const & kernel_prefix)
        {
          utils::kernel_generation_stream stream;

          for(unsigned int i = 0 ; i < mapping.size() ; ++i)
            for(mapping_type::const_iterator it = mapping[i].begin() ; it != mapping[i].end() ; ++it)
                init_simd_width(*it);

          //Generate Prototype
          std::string prototype;
          std::set<std::string> already_generated;
          add_kernel_arguments(statements, prototype);
          for(statements_container::data_type::const_iterator it = statements.data().begin() ; it != statements.data().end() ; ++it)
            tree_parsing::traverse(*it, it->root(), tree_parsing::prototype_generation_traversal(already_generated, prototype, mapping[std::distance(statements.data().begin(), it)]));
          prototype.erase(prototype.size()-1); //Last comma pruned

          for(unsigned int i = 0 ; i < num_kernels_ ; ++i)
          {
            stream << " __attribute__((reqd_work_group_size(" << local_size_0_ << "," << local_size_1_ << "," << 1 << ")))" << std::endl;
            stream << "__kernel " << "void " << kernel_prefix << i << "(" << prototype << ")" << std::endl;
            stream << "{" << std::endl;
            stream.inc_tab();
            core(i, stream, statements, mapping);
            stream.dec_tab();
            stream << "}" << std::endl;
          }

          return stream.str();
        }

      protected:
        std::string scalartype_;
        unsigned int simd_width_;
        unsigned int local_size_0_;
        unsigned int local_size_1_;

        unsigned int num_kernels_;
    };

  }

}

#endif