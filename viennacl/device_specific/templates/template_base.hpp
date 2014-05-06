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

namespace viennacl{

  namespace device_specific{


    /** @brief Base class for an operation profile */
    class profile_base{
      protected:
        virtual bool invalid_impl(viennacl::ocl::device const & /*dev*/, size_t /*scalartype_size*/) const { return false; }

        virtual std::size_t lmem_used(std::size_t /*scalartype_size*/) const { return 0; }

        void configure_local_sizes(viennacl::ocl::kernel & k, std::size_t /*kernel_id*/) const {
          k.local_work_size(0,local_size_1_);
          k.local_work_size(1,local_size_2_);
        }

        virtual void initialize_mapping(std::vector<mapping_type> & mapping) const{
            std::map<void *, std::size_t> memory;
            unsigned int current_arg = 0;
            std::size_t i = 0;
            for(std::list< std::pair<scheduler::statement, scheduler::statement_node> >::const_iterator it = statements_->begin() ; it != statements_->end() ; ++it)
              tree_parsing::traverse(it->first, it->second, tree_parsing::map_functor(memory,current_arg,mapping[i++]));
        }

        virtual void init(std::pair<scheduler::statement, scheduler::statement_node> const &, mapping_type & mapping) {
          for(mapping_type::const_iterator iit = mapping.begin() ; iit != mapping.end() ; ++iit)
              if(mapped_handle * p = dynamic_cast<mapped_handle *>(iit->second.get()))
                p->set_simd_width(simd_width_);
        }

        /** @brief Generates the body of the associated kernel function
         *
         *  @param kernel_id  If this profile requires multiple kernel, the index for which the core should be generated
         *  @param stream     The output stream the kernel is written to
         *  @param statements the statements for which the code should be generated
         *  @param mapping    the mapping of the statement_nodes to the mapped_objects
         */
        virtual void core(std::size_t kernel_id, utils::kernel_generation_stream& stream, std::vector<mapping_type> const & mapping) const = 0;

      public:
        /** @brief The constructor */
        profile_base(const char * scalartype, unsigned int simd_width, std::size_t local_size_1, std::size_t local_size_2, std::size_t num_kernels) : scalartype_(scalartype), simd_width_(simd_width), local_size_1_(local_size_1), local_size_2_(local_size_2), num_kernels_(num_kernels){ }

        void bind_statements(std::list< std::pair<scheduler::statement, scheduler::statement_node> > const * statements) { statements_ = statements; }

        unsigned int num_kernels() const { return num_kernels_; }
        /** @brief The destructor */
        virtual ~profile_base(){ }

        /** @brief Configures the range and enqueues the arguments associated with the profile */
        virtual void configure_range_enqueue_arguments(std::size_t kernel_id, viennacl::ocl::kernel & k, unsigned int & n_arg) const = 0;
        virtual void add_kernel_arguments(std::string & arguments_string) const = 0;


        /** @brief returns whether or not the profile leads to undefined behavior on particular device
         *  @param dev               the given device
         *  @param scalartype_size   Local memory required to execute the kernel
         */
        bool is_invalid(viennacl::ocl::device const & dev) const{
          //Query device informations
          size_t lmem_available = static_cast<size_t>(dev.local_mem_size());
          size_t max_workgroup_size = dev.max_work_group_size();

          std::vector<size_t> max_work_item_sizes = dev.max_work_item_sizes();
          bool invalid_work_group_sizes = local_size_1_*local_size_2_ > max_workgroup_size
              || local_size_1_ > max_work_item_sizes[0]
              || local_size_2_ > max_work_item_sizes[1]; // uses too much resources
		  
          bool not_warp_multiple = false;
          if(dev.type()==CL_DEVICE_TYPE_GPU){
            std::size_t warp_size = 32;
            if(dev.vendor_id()==4098)
              warp_size = 64;
            not_warp_multiple = static_cast<bool>(((local_size_1_*local_size_2_)%warp_size)>0);
          }

          std::size_t scalartype_size;
          if(scalartype_=="float")
            scalartype_size = 4;
          else
            scalartype_size = 8;

          return  invalid_work_group_sizes
              || lmem_used(scalartype_size)>lmem_available
              || invalid_impl(dev, scalartype_size)
              || not_warp_multiple;
        }

        /** @brief Generates the code associated with this profile onto the provided stream
         *  Redirects to the virtual core() method
         *
         *  @param stream Stream onto which the code should be generated
         */
        virtual void operator()(utils::kernel_generation_stream & stream) {
          std::vector<mapping_type> mapping(statements_->size());

          ///Get Prototype, initialize mapping
          std::string prototype;
          std::set<std::string> already_generated;
          add_kernel_arguments(prototype);
          initialize_mapping(mapping);

          for(std::list< std::pair<scheduler::statement, scheduler::statement_node> >::const_iterator it = statements_->begin() ; it != statements_->end() ; ++it){
            mapping_type & mapping_ref = mapping[std::distance(statements_->begin(), it)];
            init(*it, mapping_ref);
            tree_parsing::traverse(it->first, it->second, tree_parsing::prototype_generation_traversal(already_generated, prototype, mapping_ref));
          }

          prototype.erase(prototype.size()-1); //Last comma pruned

          //Generate
          for(std::size_t n = 0 ; n < num_kernels_ ; ++n){
            //stream << "__attribute__((vec_type_hint()))" << std::endl;
            stream << " __attribute__((reqd_work_group_size(" << local_size_1_ << "," << local_size_2_ << "," << 1 << ")))" << std::endl;
            stream << "__kernel " << "void " << "kernel_" << n << "(" << std::endl;
            stream << prototype << std::endl;
            stream << ")" << std::endl;

            //core:
            stream << "{" << std::endl;
            stream.inc_tab();
            core(n, stream, mapping);
            stream.dec_tab();
            stream << "}" << std::endl;
          }
        }

      protected:
        std::string scalartype_;
        unsigned int simd_width_;
        unsigned int local_size_1_;
        unsigned int local_size_2_;

        unsigned int num_kernels_;

        std::list< std::pair<scheduler::statement, scheduler::statement_node> > const * statements_;
    };

  }

}

#endif
