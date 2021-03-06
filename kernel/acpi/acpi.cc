/*******************************************************************************

    Copyright (C) 2014,2015  René 'Shirk' Köcher
 
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

/* ACPI interface */

#include <hotarubi/acpi/acpi.h>
#include <hotarubi/acpi/tables.h>

#include <hotarubi/processor/core.h>
#include <hotarubi/processor/pic.h>
#include <hotarubi/processor/ioapic.h>
#include <hotarubi/processor/interrupt.h>

#include <hotarubi/io.h>
#include <hotarubi/memory.h>
#include <hotarubi/log/log.h>

#include <new>
#include <iterators.h>

namespace acpi
{

/* TODO: collect the size of all tables etc. and reserve an IO-region */
//static memory::mmio::resource_t acpi_mem = nullptr;
static struct rsdt *rsdt = nullptr;
static struct xsdt *xsdt = nullptr;

static system_descriptor_table*
_get_table( const char *signature )
{
	system_descriptor_table *res = nullptr;

	if( rsdt != nullptr )
	{
		size_t max_entry  = rsdt->length - sizeof( struct rsdt ) + sizeof( rsdt->entries[0] );
		       max_entry /= sizeof( rsdt->entries[0] );

		for( size_t i = 0; i < max_entry; ++i )
		{
			res = ( system_descriptor_table* )__VA( rsdt->entries[i] );
			if( res->check( signature ) )
			{
				break;
			}
			else
			{
				res = nullptr;
			}
		} 
	}
	else if( xsdt != nullptr )
	{
		size_t max_entry  = xsdt->length - sizeof( struct xsdt ) + sizeof( xsdt->entries[0] );
		       max_entry /= sizeof( xsdt->entries[0] );

		for( size_t i = 0; i < max_entry; ++i )
		{
			res = ( system_descriptor_table* )__VA( xsdt->entries[i] );
			if( res->check( signature ) )
			{
				break;
			}
			else
			{
				res = nullptr;
			}
		} 
	}
	return res;
}

/* iteration support on madt tables */
static auto madt_add = []( uintptr_t& ptr ) {
	ptr += ( ( madt_entry* )ptr )->length;
};

static auto madt_get = []( uintptr_t ptr ) {
	return ( madt_entry* )ptr;
};

calc_iter<madt_entry> begin( struct madt& madt )
{
	return calc_iter<madt_entry>( madt_add, madt_get, &madt.entries[0] );
};

calc_iter<madt_entry> end( struct madt& madt )
{
	return calc_iter<madt_entry>(
	  madt_add, madt_get,
	  ( void * )( ( ( uintptr_t )&madt ) + madt.length ) );
};

/* ---- */

void
parse_madt( processor::core *&aps, uint32_t &core_count,
            processor::ioapic *&ioapics, uint32_t &ioapic_count  )
{
	auto madt = ( struct madt* )_get_table( ACPI_MADT_SIG );
	if( madt == nullptr )
	{
		panic( "acpi: No MADT table available!" );
	}

	if( madt->flags & 1 )
	{
		log::printk( "acpi: disabling 8259 PICs\n" );
		processor::pic::initialize();
		processor::pic::disable();
	}

	core_count = 0;
	ioapic_count = 0;

	uint32_t lapic_base = 0;
	uint32_t core_base = 255;
	
	/* first run: count the system resources */
	for( auto entry : *madt )
	{
		switch( entry->type )
		{
			case MADTEntryType::kLAPIC:
			{
				++core_count;
			
				auto desc = ( madt_lapic_entry* )entry;
				core_base = ( desc->processor_id < core_base ) ? desc->processor_id : core_base;
				break;
			}

			case MADTEntryType::kIOAPIC:
				++ioapic_count;
				break;

			case MADTEntryType::kSourceOverride:
			{
				auto desc     = ( madt_source_override* )entry;
				auto trigger  = processor::TriggerMode( desc->flags.trigger );
				auto polarity = processor::Polarity( desc->flags.polarity );
				auto irq      = processor::core::irqs( desc->source_irq );
				if( irq != nullptr )
				{
					irq->setup( desc->source_irq, desc->global_irq,
				                trigger, polarity );
				}
				break;
			}

			case MADTEntryType::kLAPIAddressOverride:
			{
				auto desc = ( madt_lapic_address_override* )entry;
				lapic_base = desc->lapic_base;
				break;
			}

			default:
				break;
		}
	}

	if( core_count > 0 )
	{
		log::printk( "acpi: detected %i processors\n", core_count );
		aps = new( std::nothrow ) processor::core[core_count - 1];
	}
	else
	{
		panic( "acpi: no processor detected - wait WHAT!?" );
	}
	if( ioapic_count > 0 )
	{
		log::printk( "acpi: detected %i IOAPICs\n", ioapic_count );
		ioapics = new( std::nothrow ) processor::ioapic[ioapic_count];
	}
	else
	{
		panic( "acpi: no IOAPICs - wait WHAT!?" );
	}

	/* second run: configure them */
	ioapic_count = 0;
	for( auto entry : *madt )
	{
		switch( entry->type )
		{
			case MADTEntryType::kLAPIC:
			{
				auto desc   = ( madt_lapic_entry* )entry;
				auto lapic  = new( std::nothrow ) processor::lapic( desc->apic_id, lapic_base );
				
				processor::core::instance( desc->processor_id - core_base )->lapic = lapic;
				log::printk( "acpi: detected LAPIC %d for processor %d\n",
			                 desc->apic_id, desc->processor_id );
				break;
			}

			case MADTEntryType::kIOAPIC:
			{
				auto desc   = ( madt_ioapic_entry* )entry;
				auto ioapic = &ioapics[ioapic_count++];

				ioapic->init( desc->address, desc->base_irq );
				log::printk( "acpi: detected IOAPIC %d, version %d handling IRQ %02x => %02x\n",
				             ioapic->id(), ioapic->version(),
				             ioapic->start(), ioapic->range() );
				break;
			}

			default:
				break;
		}
	}

	for( auto i = 0; i < NUM_IRQ_ENTRIES; ++i )
	{
		auto irq = processor::core::irqs( i );
		if( irq->override() )
		{
			log::printk( "acpi: override IRQ %02x => %02x, %2s, %2s\n",
			             i, irq->target,
			             irq->str( irq->trigger ),
			             irq->str( irq->polarity ) );
		}
	}

	/* third run: collect and apply global and lapic nmi overrides */
	for( auto entry : *madt )
	{
		switch( entry->type )
		{
			case MADTEntryType::kNMISource:
			{
				auto desc = ( madt_nmi_source* )entry;
				auto trigger  = processor::TriggerMode( desc->flags.trigger );
				auto polarity = processor::Polarity( desc->flags.polarity );

				log::printk( "acpi: IRQ %02x, global NMI\n", desc->global_irq );
				if( processor::core::set_nmi( desc->global_irq, trigger, polarity ) == false )
				{
					log::printk( "acpi: no IOAPIC available to handle this NMI!\n" );
				}
				break;
			}

			case MADTEntryType::kLAPICNMI:
			{
				auto desc = ( madt_lapic_nmi* )entry;
				auto trigger  = processor::TriggerMode( desc->flags.trigger );
				auto polarity = processor::Polarity( desc->flags.polarity );

				log::printk( "acpi: LINT%d, NMI ", desc->lint);
				if( desc->processor_id == 255 )
				{
					log::printk( " on all LAPICs\n" );
					for( uint32_t i = 0; i < core_count; ++i )
					{
						auto lapic = processor::core::instance( i )->lapic;

						if( lapic != nullptr )
						{
							lapic->set_nmi( desc->lint, trigger, polarity );
						}
					}
				}
				else
				{
					if( desc->processor_id - core_base > core_count )
					{
						log::printk( " -- with unknown procossor %d\n",
						             desc->processor_id );
						break;
					}

					auto lapic = processor::core::instance( desc->processor_id - core_base )->lapic;
					if( lapic != nullptr )
					{
						log::printk( " on processor %d\n", desc->processor_id );
						lapic->set_nmi( desc->lint, trigger, polarity );
					}
				}
				break;
			}

			default:
				break;
		}
	}
}

void
init_tables( void )
{
	/* locate the rsdp in low mem (reading the EBDA location from BDA:0x40e ) */
	/* TODO: this won't work on UEFI */
	uint8_t *mem = ( uint8_t* )__VA( 0x00000000 );

	log::printk( "Parsing ACPI tables..\n" );
	for( size_t i = ( mem[0x040e] >> 4 ); i < 0x100000; i += 2 )
	{
		auto rsd_ptr = ( rsdp* )&mem[i];
		if( rsd_ptr->check() )
		{
			if( rsd_ptr->xsdt_ptr == 0 )
			{
				log::printk( "acpi: no valid XSDT, using RSDT\n" );
				rsdt = ( struct rsdt* )__VA( rsd_ptr->rsdt_ptr );
				if( !rsdt->check( ACPI_RSDT_SIG ) )
				{
					panic( "acpi: RSDT is INVALID!" );
				}
				log::printk( "acpi: RSDT version %d (%6.6s)\n",
				             rsdt->revision, rsdt->oem_id );
				break;
			}
			else
			{
				xsdt = ( struct xsdt* )__VA( rsd_ptr->xsdt_ptr );
				if( !xsdt->check( ACPI_XSDT_SIG ) )
				{
					panic( "acpi: XSDT is INVALID!" );
				}
				log::printk( "acpi: XSDT version %d (%6.6s)\n",
				             xsdt->revision, xsdt->oem_id );
				break;
			}
		}
		rsd_ptr = nullptr;
	}
}

};
