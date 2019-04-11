////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source
// License.  See LICENSE file in top directory for details.
//
// Copyright (c) 2019 QMCPACK developers.
//
// File developed by:
// Peter Doak, doakpw@ornl.gov, Oak Ridge National Lab
//
// File created by:
// Peter Doak, doakpw@ornl.gov, Oak Ridge National Lab
////////////////////////////////////////////////////////////////////////////////

#ifndef QMCPLUSPLUS_MINIQMC_DRIVER_FUNCTIONS_HPP
#define QMCPLUSPLUS_MINIQMC_DRIVER_FUNCTIONS_HPP

#include <functional>
#include <boost/hana/map.hpp>
//#include <boost/thread/barrier.hpp>
#include "Devices.h"
#include "Drivers/MiniqmcOptions.hpp"
#include "Drivers/Mover.hpp"
#include "Drivers/Crowd.hpp"
#include "Particle/ParticleSet.h"
#include "Input/nio.hpp"
#include "QMCWaveFunctions/Determinant.h"
#include "QMCWaveFunctions/DeterminantDeviceImp.h"
#include "QMCWaveFunctions/SPOSet.h"
#include "QMCWaveFunctions/SPOSet_builder.h"
#include "Utilities/TaskBlock.hpp"
#include "Utilities/qmcpack_version.h"
#include "Utilities/PrimeNumberSet.h"

namespace qmcplusplus
{

/** A purely functional class implementing miniqmcdriver functions
 *  functions can be specialized for a particular device
 *  drive steps are clearly broken up.
 */

template<Devices DT>
class MiniqmcDriverFunctions
{
public:
  using QMCT = QMCTraits;
  static void initialize(int arc, char** argv);
  static void buildSPOSet(SPOSet*& spo_set,
			  MiniqmcOptions& mq_opt,
			  const int norb,
			  const int nTiles,
			  const int tile_size,
			  const Tensor<OHMMS_PRECISION, 3>& lattice_b);
  static void runThreads(MiniqmcOptions& mq_opt,
                         const PrimeNumberSet<uint32_t>& myPrimes,
                         ParticleSet& ions,
			 const SPOSet* spo_main);
  static void movers_runThreads(MiniqmcOptions& mq_opt,
                         const PrimeNumberSet<uint32_t>& myPrimes,
                         ParticleSet& ions,
				const SPOSet* spo_main);
  // static void movers_runStdThreads(MiniqmcOptions& mq_opt,
  //                        const PrimeNumberSet<uint32_t>& myPrimes,
  //                        ParticleSet& ions,
  // 			 const SPOSet* spo_main);

  static void finalize();
private:
  static void mover_info();
  template<Threading TT>
  static void movers_thread_main(const int ip,
				 TaskBlockBarrier<TT>& barrier,
			  const int team_size,
			  MiniqmcOptions& mq_opt,
                          const PrimeNumberSet<uint32_t>& myPrimes,
                          ParticleSet ions,
				 const SPOSet* spo_main);
//      				boost::barrier& block_complete);
  static void thread_main(const int ip,
			  const int team_size,
			  MiniqmcOptions& mq_opt,
                          const PrimeNumberSet<uint32_t>& myPrimes,
                          ParticleSet ions,
			  const SPOSet* spo_main);
  static void updateFromDevice(DiracDeterminant<DeterminantDeviceImp<DT>>& determinant_device);
};

template<Devices DT>
void MiniqmcDriverFunctions<DT>::buildSPOSet(SPOSet*& spo_set,
                                             MiniqmcOptions& mq_opt,
                                             const int norb,
                                             const int nTiles,
					     const int tile_size,
                                             const Tensor<OHMMS_PRECISION, 3>& lattice_b)
{
  spo_set =
      SPOSetBuilder<DT>::build(mq_opt.useRef, mq_opt.nx, mq_opt.ny, mq_opt.nz, norb, nTiles, tile_size, lattice_b);
}

/** thread main using Crowd
 *  SINGLE should really be a case of Crowd of size 1
 *  but lets work through this first
 */

template<Devices DT>
template<Threading TT>
void MiniqmcDriverFunctions<DT>::movers_thread_main(const int ip,
						    				    TaskBlockBarrier<TT>& barrier,
                                                              const int team_size,
                                                              MiniqmcOptions& mq_opt,
                                                              const PrimeNumberSet<uint32_t>& myPrimes,
                                                              ParticleSet ions,
						    const SPOSet* spo_main)
{
  const int member_id = ip % team_size;
  // create and initialize movers
  //app_summary() << "pack size:" << mq_opt.pack_size << '\n';
  app_summary() << "thread:" << ip << " starting up \n";
  int my_accepts = 0;
  Crowd<DT> movers(ip, myPrimes, ions, mq_opt.pack_size);

  movers.init();
  // For VMC, tau is large and should result in an acceptance ratio of roughly
  // 50%
  // For DMC, tau is small and should result in an acceptance ratio of 99%
  
  const QMCT::RealType tau = 2.0;

  QMCT::RealType sqrttau = std::sqrt(tau);

  
  // create a spo view in each Mover
  movers.buildViews(mq_opt.useRef, spo_main, team_size, member_id);

  movers.buildWaveFunctions(mq_opt.useRef, mq_opt.enableJ3);

  // initial update
  std::for_each(movers.elss_begin(), movers.elss_end(), [](ParticleSet& els) { els.update(); });
  // for(auto& els_it = movers.elss_begin(); els_it != movers.elss_end(); els_it++)
  //   {
  //     els.epdate();
  //   }

  movers.evaluateLog();

  //app_summary() << "initial update complete \n";

  const int nions = ions.getTotalNum();
  const int nels  = movers.elss[0]->getTotalNum();
  const int nels3 = 3 * nels;

  // this is the number of quadrature points for the non-local PP
  const int nknots(movers.nlpps[0]->size());

  for (int mc = 0; mc < mq_opt.nsteps; ++mc)
  {
    mq_opt.Timers[Timer_Diffusion]->start();

    for (int l = 0; l < mq_opt.nsubsteps; ++l) // drift-and-diffusion
    {
      movers.fillRandoms();
      
      for (int iel = 0; iel < nels; ++iel)
      {
        // Operate on electron with index iel
        // probably should be in movers
        std::for_each(movers.elss_begin(), movers.elss_end(), [iel](ParticleSet& els) { els.setActive(iel); });

        // Compute gradient at the current position
        mq_opt.Timers[Timer_evalGrad]->start();
        movers.evaluateGrad(iel);
        mq_opt.Timers[Timer_evalGrad]->stop();

        movers.constructTrialMoves(iel);

	// Compute gradient at the trial position 
        mq_opt.Timers[Timer_ratioGrad]->start();
	movers.evaluateRatioGrad(iel);
        mq_opt.Timers[Timer_ratioGrad]->stop();


	mq_opt.Timers[Timer_evalVGH]->start();
	movers.evaluateHessian(iel);
	mq_opt.Timers[Timer_evalVGH]->stop();

	mq_opt.Timers[Timer_Update]->start();
	movers.finishUpdate(iel);
        mq_opt.Timers[Timer_Update]->stop();

  
	// Accept/reject the trial move
        mq_opt.Timers[Timer_Update]->start();
	int these_accepts = movers.acceptRestoreMoves(iel, mq_opt.accept);
	//app_summary() << "Moves accepted: " << these_accepts << "\n";
	my_accepts += these_accepts;
        mq_opt.Timers[Timer_Update]->stop();
      }//iel
    }//substeps
    movers.donePbyP();
    movers.evaluateGL();

    mq_opt.Timers[Timer_ECP]->start();

    mq_opt.Timers[Timer_Value]->start();
    movers.calcNLPP(nions, mq_opt.Rmax);
    mq_opt.Timers[Timer_Value]->stop();
    mq_opt.Timers[Timer_ECP]->stop();

    mq_opt.Timers[Timer_Diffusion]->stop();
  } // nsteps
  barrier.wait();
}

extern template class qmcplusplus::MiniqmcDriverFunctions<Devices::CPU>;
#ifdef QMC_USE_KOKKOS
extern template class qmcplusplus::MiniqmcDriverFunctions<Devices::KOKKOS>;
#endif
#ifdef QMC_USE_CUDA
extern template class qmcplusplus::MiniqmcDriverFunctions<Devices::CUDA>;
#endif

} // namespace qmcplusplus

#endif
