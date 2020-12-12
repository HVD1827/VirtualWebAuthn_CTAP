/******************************************************************************
* File:        Web_authn_tpm.cpp
* Description: Implementation of the Web_authn_tpm class for the authenticator
*
* Author:      Chris Newton
*
* Created:     Wednesday 9 December 2020
*
* (C) Copyright 2020, University of Surrey, all rights reserved.
*
******************************************************************************/

#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include "Tss_includes.h"
#include "Make_key_persistent.h"
#include "Flush_context.h"
#include "Io_utils.h"
#include "Tpm_error.h"
#include "Create_primary_rsa_key.h"
#include "Create_ecdsa_key.h"
#include "Openssl_ec_utils.h"
#include "Clock_utils.h"
#include "Tss_setup.h"
#include "Tpm_initialisation.h"
#include "Tpm_defs.h"
#include "Tpm_param.h"
#include "Byte_array.h"
#include "Web_authn_tpm.h"


TPM_RC Web_authn_tpm::setup(Tss_setup const& tps, std::string log_file)
{
	TPM_RC rc=0;
	try
	{
		std::string filename=generate_log_filename(tps.data_dir.value, log_file);
		log_ptr_.reset(new Timed_file_log(filename));
		log_ptr_->set_debug_level(dbg_level_);
		log(1,"TPM setup started");

        hw_tpm_=(tps.t==Tpm_type::device);
        if (!hw_tpm_)
        {
            rc=powerup(tps);
            if (rc!=0)
            {
				log(1,"Web_authn_tpm: setup: Simulator powerup failed");
                throw(Tpm_error("Simulator powerup failed\n"));
            }
        }

        auto nc=set_new_context(tps);
        rc=nc.first;
        if (rc!=0)
        {
			log(1,"Web_authn_tpm: setup: failed to create a TSS context");
            throw(Tpm_error("Web_authn_tpm: setup: failed to create a TSS context\n"));
        }
        tss_context_=nc.second;

		rc=startup(tss_context_);
		if (rc!=0 && rc!=TPM_RC_INITIALIZE)
		{
			shutdown(tss_context_);
			log(1,"Web_authn_tpm: setup: TPM startup failed (reset the TPM)");
			throw(Tpm_error("TPM startup failed (reset the TPM)"));
		}

        if (!persistent_key_available(tss_context_,srk_persistent_handle)) {
	       	uint32_t object_attributes = TPMA_OBJECT_FIXEDTPM |		// TPMA_OBJECT is a bit field
					    TPMA_OBJECT_FIXEDPARENT |
    		       		TPMA_OBJECT_SENSITIVEDATAORIGIN |
				       	TPMA_OBJECT_USERWITHAUTH |
                       	TPMA_OBJECT_RESTRICTED |
                       	TPMA_OBJECT_NODA |
				       	TPMA_OBJECT_DECRYPT;
			CreatePrimary_Out out;
			rc=create_primary_rsa_key(tss_context_,TPM_RH_OWNER,object_attributes,Byte_buffer(),&out);
			log(1,"Primary key created");
			rc=make_key_persistent(tss_context_,out.objectHandle,srk_persistent_handle);
			log(1,"Primary key made persistent");
		} else {
			log(1,"Primary key already installed");
		}

        rc=TSS_Delete(tss_context_);
        tss_context_=nullptr;
    }
	catch (Tpm_error &e)
	{
		rc=1;
		last_error_=vars_to_string("Web_authn_tpm: setup: Tpm_error: ",e.what());
	}
	catch(std::runtime_error &e)
	{
		rc=2;
		last_error_=vars_to_string("Web_authn_tpm: setup: runtime_error: ", e.what());
	}
	catch (...)
	{
		rc=3;
		last_error_="Web_authn_tpm: setup: failed - uncaught exception";
	}

	log(1,"TPM setup complete");

	return rc;
}

/*
TPM_RC Web_authn_tpm::get_endorsement_key_data(Byte_buffer& ek_pd)
{
	TPM_RC rc=0;

	try
	{
		if (log_ptr->debug_level()>0)
		{
			log_ptr->write_to_log("Web_authn_tpm: get_endorsement_key_data\n");
		}

		Byte_buffer pd=key_store_.public_data_bb("ek");
		if (pd.size()==0)
		{
			log_ptr->write_to_log("Web_authn_tpm: get_endorsement_key_data: marshalling public data failed\n");
			throw(Tpm_error("Marshalling EK public data failed"));
		}
		ek_pd=pd;
	}
	catch (Tpm_error &e)
	{
		rc=1;
		last_error_=std::string(e.what());
	}
	catch (...)
	{
		rc=2;
		last_error_="Failed - uncaught exception";
	}

	if (log_ptr->debug_level()>0)
	{
		log_ptr->os() << "Web_authn_tpm: get_endorsement_key_data returned:\npd: " << ek_pd.to_hex_string() << std::endl;
	}
	return rc;
}
*/


std::string Web_authn_tpm::get_last_error()
{
	// Move the contents of last_error also clears the value
	std::string error(std::move(last_error_));
	last_error_="No error";
	return error;
}

void Web_authn_tpm::release_memory()
{
	release_byte_array(kd_.public_data);
	release_byte_array(kd_.private_data);
	release_byte_array(pt_.x_coord);
	release_byte_array(pt_.y_coord);
	release_byte_array(signing_data_);
	release_byte_array(sig_.sig_r);
	release_byte_array(sig_.sig_s);
	
	// Now the temporary data
	release_byte_array(ba_);
	release_byte_array(tba_.one);
	release_byte_array(tba_.two);
}

void Web_authn_tpm::log(int dbg_level, std::string const& str)
{
	if (dbg_level<dbg_level_) {
		return;
	}
	log_ptr_->os() << str << '\n';
}

Web_authn_tpm::~Web_authn_tpm()
{
	if (tss_context_)
	{
		// Assumes all keys have bee flushed
		shutdown(tss_context_);
		TSS_Delete(tss_context_);
	}

	release_memory();
}

void ba_copy(Byte_array& lhs, Byte_array const& rhs)
{
	if (&lhs!=&rhs)
	{
		release_byte_array(lhs);		
		lhs.size=rhs.size;
		lhs.data=new Byte[lhs.size];
		if (lhs.data==nullptr)
		{
			std::cerr << "Unable to aloocate memory the a Byte_array\n";
			exit(1);
		}
		memcpy(lhs.data,rhs.data,rhs.size);
	}
}

void tba_copy(Two_byte_arrays&lhs, Two_byte_arrays const& rhs)
{
	ba_copy(lhs.one,rhs.one);
	ba_copy(lhs.two,rhs.two);
}

// Temporary member functions for testing
Byte_array Web_authn_tpm::get_byte_array()
{
	return ba_;	
}

void Web_authn_tpm::put_byte_array(Byte_array ba)
{
	ba_copy(ba_,ba);
}

Two_byte_arrays Web_authn_tpm::get_two_byte_arrays()
{
	return tba_;
}

void Web_authn_tpm::put_two_byte_arrays(Two_byte_arrays tba)
{
	tba_copy(tba_,tba);
}
