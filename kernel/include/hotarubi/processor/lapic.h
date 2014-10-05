/*******************************************************************************

    Copyright (C) 2014  René 'Shirk' Köcher
 
    This file is part of Hotarubi.

    Hotarubi is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
 
    Hotarubi is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*******************************************************************************/

/* LAPIC handling */

#ifndef __PROCESSOR_LAPIC_H
#define __PROCESSOR_LAPIC_H 1

#include <stdint.h>
#include <hotarubi/memory/mmio.h>
#include <hotarubi/processor/interrupt.h>

namespace processor
{
	/* we need to:
	 * - configure lint# as NMI
	 * - calibrate the timer frequency
	 * - start single-shot and repeating timers
	 * - stop a running timer
	 * - mask and unmask interrupts
	 * - send an IPI [to another LAPIC|to shorthand addresses]
	 */

	class lapic
	{
	public:
		enum LAPICInterrupt
		{
			kLAPICInterruptLINT0   = 1,
			kLAPICInterruptLINT1   = 2,
			kLAPICInterruptTimer   = 3,
			kLAPICInterruptPerfMon = 4,
			kLAPICInterruptThermal = 5,
			kLAPICInterruptError   = 6,
		};

		lapic( uint8_t id, uint32_t address );
		~lapic();

		void init();

		void calibrate( void );

		void set_route( LAPICInterrupt source, uint8_t target,
		                IRQTriggerMode trigger=kIRQTriggerConform, 
		                IRQPolarity polarity=kIRQPolarityConform );
		void set_mask( LAPICInterrupt source, bool masked );

		void set_timer( unsigned msec, bool repeat=false );
		void clr_timer( void );

		void set_nmi( unsigned lint_no, IRQTriggerMode trigger, IRQPolarity polarity );
		void clr_nmi( unsigned lint_no );

		void eoi( void );

		uint8_t id( void ) const { return _id; };
		uint8_t version( void ) const { return _version; };

	private:
		uint32_t _irq_flags( IRQTriggerMode trigger, IRQPolarity polarity );

		uint32_t _read( uint16_t reg );
		void _write( uint16_t reg, uint32_t val );

		uint8_t                  _id        = 0;
		uint8_t                  _version   = 0;
		uint32_t                 _ticks_per_msec = 1;

		memory::mmio::resource_t _io_mem  = nullptr;
		uintptr_t                _io_base = 0;

		/* initialization data */
		bool                     _init_done = false;

		uint8_t                  _init_id   = 0;
		uint32_t                 _init_addr = 0;
		unsigned                 _init_nmi  = 0xff;
		IRQTriggerMode           _init_nmi_trigger  = kIRQTriggerConform;
		IRQPolarity              _init_nmi_polarity = kIRQPolarityConform;
	};
};

#endif
