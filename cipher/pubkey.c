/* pubkey.c  -	pubkey dispatcher
 * Copyright (C) 1998,1999,2000,2002,2003 Free Software Foundation, Inc.
 *
 * This file is part of Libgcrypt.
 *
 * Libgcrypt is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser general Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * Libgcrypt is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "g10lib.h"
#include "mpi.h"
#include "cipher.h"
#include "ath.h"

/* FIXME: use set_lasterr() */

static struct
{
  const char* name;
  int algo;
  const char* common_elements;
  const char* public_elements;
  const char* secret_elements;
  const char* grip_elements;
} algo_info_table[] = {
  { "dsa"        ,      GCRY_PK_DSA       , "pqgy", "", "x",    "pqgy" },
  { "rsa"        ,      GCRY_PK_RSA       , "ne",   "", "dpqu", "n" },
  { "elg"        ,      GCRY_PK_ELG   , "pgy",  "", "x",    "pgy"  },
  { "openpgp-dsa",      GCRY_PK_DSA       , "pqgy", "", "x",    "pqgy" },
  { "openpgp-rsa",      GCRY_PK_RSA       , "ne",   "", "dpqu"  "n"},
  { "openpgp-elg",      GCRY_PK_ELG_E , "pgy",  "", "x",    "pgy" },
  { "openpgp-elg-sig",  GCRY_PK_ELG   , "pgy",  "", "x",    "pgy" },
  { "oid.1.2.840.113549.1.1.1",
                        GCRY_PK_RSA       , "ne",   "", "dpqu", "n" },
  { NULL }
};

static struct {
    const char* name; int algo;
    const char* elements;
} sig_info_table[] = {
  { "dsa"                     , GCRY_PK_DSA       , "rs" },
  { "rsa"                     , GCRY_PK_RSA       , "s"  },
  { "elg"                     , GCRY_PK_ELG   , "rs" },
  { "openpgp-dsa"             , GCRY_PK_DSA       , "rs" },
  { "openpgp-rsa"             , GCRY_PK_RSA       , "s"  },
  { "openpgp-elg-sig"         , GCRY_PK_ELG   , "rs" },
  { "oid.1.2.840.113549.1.1.1", GCRY_PK_RSA       , "s"  },
  { NULL }
};

static struct {
    const char* name; int algo;
    const char* elements;
} enc_info_table[] = {
  { "elg"            ,          GCRY_PK_ELG   , "ab" },
  { "rsa"            ,          GCRY_PK_RSA       , "a"  },
  { "openpgp-rsa"    ,          GCRY_PK_RSA       , "a"  },
  { "openpgp-elg"    ,          GCRY_PK_ELG_E , "ab" },
  { "openpgp-elg-sig",          GCRY_PK_ELG   , "ab" },
  { "oid.1.2.840.113549.1.1.1", GCRY_PK_RSA       , "a"  },
  { NULL }
};

static int pubkey_decrypt( int algo, MPI *result, MPI *data, MPI *skey, int flags);
static int pubkey_sign( int algo, MPI *resarr, MPI hash, MPI *skey );
static int pubkey_verify( int algo, MPI hash, MPI *data, MPI *pkey,
		      int (*cmp)(void *, MPI), void *opaque );

/* This is the list of the default public-key ciphers included in
   libgcrypt.  */
static struct
{
  GcryPubkeySpec *pubkey;
} pubkey_table[] =
  {
#if USE_RSA
    { &pubkey_spec_rsa },
#endif
#if USE_ELGAMAL
    { &pubkey_spec_elg },
#endif
#if USE_DSA
    { &pubkey_spec_dsa },
#endif
    { NULL },
  };

/* List of registered ciphers.  */
static GcryModule *pubkeys_registered;

/* This is the lock protecting PUBKEYS_REGISTERED.  */
static ath_mutex_t pubkeys_registered_lock;

/* Flag to check wether the default pubkeys have already been
   registered.  */
static int default_pubkeys_registered;

/* Convenient macro for registering the default digests.  */
#define REGISTER_DEFAULT_PUBKEYS                   \
  do                                               \
    {                                              \
      ath_mutex_lock (&pubkeys_registered_lock);   \
      if (! default_pubkeys_registered)            \
        {                                          \
          gcry_pubkey_register_default ();         \
          default_pubkeys_registered = 1;          \
        }                                          \
      ath_mutex_unlock (&pubkeys_registered_lock); \
    }                                              \
  while (0)

/* These dummy functions are used in case a cipher implementation
   refuses to provide it's own functions.  */

static int
dummy_generate (int id, unsigned int nbits, unsigned long dummy,
                MPI *skey, MPI **retfactors)
{
  log_bug ("no generate() for %d\n", id);
  return GCRYERR_INV_PK_ALGO;
}

static int
dummy_check_secret_key (int id, MPI *skey)
{
  log_bug ("no check_secret_key() for %d\n", id);
  return GCRYERR_INV_PK_ALGO;
}

static int
dummy_encrypt (int id, MPI *resarr, MPI data, MPI *pkey, int flags)
{
  log_bug ("no encrypt() for %d\n", id);
  return GCRYERR_INV_PK_ALGO;
}

static int
dummy_decrypt (int id, MPI *result, MPI *data, MPI *skey, int flags)
{
  log_bug ("no decrypt() for %d\n", id);
  return GCRYERR_INV_PK_ALGO;
}

static int
dummy_sign (int id, MPI *resarr, MPI data, MPI *skey)
{
  log_bug ("no sign() for %d\n", id);
  return GCRYERR_INV_PK_ALGO;
}

static int
dummy_verify (int id, MPI hash, MPI *data, MPI *pkey,
	      int (*cmp)(void *, MPI), void *opaquev)
{
  log_bug ("no verify() for %d\n", id);
  return GCRYERR_INV_PK_ALGO;
}

static unsigned
dummy_get_nbits (int id, MPI *pkey)
{
  log_bug ("no get_nbits() for %d\n", id);
  return 0;
}


/* Internal function.  Register all the pubkeys included in
   PUBKEY_TABLE.  Returns zero on success or an error code.  */
static void
gcry_pubkey_register_default (void)
{
  int i, err = 0;
  
  for (i = 0; (! err) && pubkey_table[i].pubkey; i++)
    {
      if (! pubkey_table[i].pubkey->generate)
	pubkey_table[i].pubkey->generate = dummy_generate;
      if (! pubkey_table[i].pubkey->check_secret_key)
	pubkey_table[i].pubkey->check_secret_key = dummy_check_secret_key;
      if (! pubkey_table[i].pubkey->encrypt)
	pubkey_table[i].pubkey->encrypt = dummy_encrypt;
      if (! pubkey_table[i].pubkey->decrypt)
	pubkey_table[i].pubkey->decrypt = dummy_decrypt;
      if (! pubkey_table[i].pubkey->sign)
	pubkey_table[i].pubkey->sign = dummy_sign;
      if (! pubkey_table[i].pubkey->verify)
	pubkey_table[i].pubkey->verify = dummy_verify;
      if (! pubkey_table[i].pubkey->get_nbits)
	pubkey_table[i].pubkey->get_nbits = dummy_get_nbits;

      err = _gcry_module_add (&pubkeys_registered,
			      (void *) pubkey_table[i].pubkey,
			      NULL);
    }

  if (err)
    BUG ();
}

/* Internal callback function.  Used via _gcry_module_lookup.  */
static int
gcry_pubkey_lookup_func_id (void *spec, void *data)
{
  GcryPubkeySpec *pubkey = (GcryPubkeySpec *) spec;
  int id = *((int *) data);

  return (pubkey->id == id);
}

/* Internal callback function.  Used via _gcry_module_lookup.  */
static int
gcry_pubkey_lookup_func_name (void *spec, void *data)
{
  GcryPubkeySpec *pubkey = (GcryPubkeySpec *) spec;
  char *name = (char *) data;

  return (! stricmp (pubkey->name, name));
}

/* Internal function.  Lookup a pubkey entry by it's ID.  */
static GcryModule *
gcry_pubkey_lookup_id (int id)
{
  GcryModule *pubkey;

  pubkey = _gcry_module_lookup (pubkeys_registered, (void *) &id,
				gcry_pubkey_lookup_func_id);

  return pubkey;
}

/* Internal function.  Lookup a pubkey entry by it's name.  */
static GcryModule *
gcry_pubkey_lookup_name (const char *name)
{
  GcryModule *pubkey;

  pubkey = _gcry_module_lookup (pubkeys_registered, (void *) name,
				gcry_pubkey_lookup_func_name);

  return pubkey;
}

/* Return a new, unused pubkey ID for a user-provided pubkey
   implementation.  */
static int
gcry_pubkey_id_new (void)
{
  int id, id_start = 500, id_end = 600;	/* FIXME.  */
  
  for (id = id_start; id < id_end; id++)
    if (! gcry_pubkey_lookup_id (id))
      return id;

  return 0;
}

/* Public function.  Register a provided PUBKEY.  Returns zero on
   success, in which case the chosen pubkey ID has been stored in
   PUBKEY, or an error code.  */
int
gcry_pubkey_register (GcryPubkeySpec *pubkey, GcryModule **module)
{
  int id, err = 0;
  GcryModule *mod;

  ath_mutex_lock (&pubkeys_registered_lock);
  id = gcry_pubkey_id_new ();
  if (! id)
    err = GCRYERR_INTERNAL;	/* FIXME.  */
  else
    {
      pubkey->id = id;
      err = _gcry_module_add (&pubkeys_registered, (void *) pubkey,
			      &mod);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  if (! err)
    *module = mod;

  return err;
}

/* Public function.  Unregister the pubkey identified by ID, which
   must have been registered with gcry_pubkey_register.  */
void
gcry_pubkey_unregister (GcryModule *module)
{
  ath_mutex_lock (&pubkeys_registered_lock);
  _gcry_module_release (module);
  ath_mutex_unlock (&pubkeys_registered_lock);
}

static void
release_mpi_array (MPI *array)
{
  for (; *array; array++)
    {
      mpi_free(*array);
      *array = NULL;
    }
}

/****************
 * Map a string to the pubkey algo
 */
int
gcry_pk_map_name (const char *string)
{
  GcryModule *pubkey;
  int id = 0;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_name (string);
  if (pubkey)
    {
      id = ((GcryPubkeySpec *) pubkey->spec)->id;
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  return id;
}


/****************
 * Map a pubkey algo to a string
 */
const char *
gcry_pk_algo_name (int id)
{
  const char *name = NULL;
  GcryModule *pubkey;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_id (id);
  if (pubkey)
    {
      name = ((GcryPubkeySpec *) pubkey->spec)->name;
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  return name;
}


static void
disable_pubkey_algo (int id)
{
  GcryModule *pubkey;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_id (id);
  if (pubkey)
    {
      if (! (pubkey-> flags & FLAG_MODULE_DISABLED))
	pubkey->flags |= FLAG_MODULE_DISABLED;
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);
}


/****************
 * a use of 0 means: don't care
 */
static int
check_pubkey_algo (int id, unsigned use)
{
  GcryPubkeySpec *pubkey;
  GcryModule *module;
  int err = 0;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  module = gcry_pubkey_lookup_id (id);
  if (module)
    {
      pubkey = (GcryPubkeySpec *) module->spec;

      if (((use & GCRY_PK_USAGE_SIGN)
	   && (! (pubkey->use & GCRY_PK_USAGE_SIGN)))
	  || ((use & GCRY_PK_USAGE_ENCR)
	      && (! (pubkey->use & GCRY_PK_USAGE_ENCR))))
	err = GCRYERR_WRONG_PK_ALGO;
      else if (module->flags & FLAG_MODULE_DISABLED)
	err = GCRYERR_INV_PK_ALGO;
      _gcry_module_release (module);
    }
  else
    err = GCRYERR_INV_PK_ALGO;
  ath_mutex_unlock (&pubkeys_registered_lock);

  return err;
}


/****************
 * Return the number of public key material numbers
 */
static int
pubkey_get_npkey (int id)
{
  GcryModule *pubkey;
  int npkey = 0;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_id (id);
  if (pubkey)
    {
      npkey = ((GcryPubkeySpec *) pubkey->spec)->npkey;
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  return npkey;
}

/****************
 * Return the number of secret key material numbers
 */
static int
pubkey_get_nskey (int id)
{
  GcryModule *pubkey;
  int nskey = 0;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_id (id);
  if (pubkey)
    {
      nskey = ((GcryPubkeySpec *) pubkey->spec)->nskey;
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  return nskey;
}

/****************
 * Return the number of signature material numbers
 */
static int
pubkey_get_nsig (int id)
{
  GcryModule *pubkey;
  int nsig = 0;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_id (id);
  if (pubkey)
    {
      nsig = ((GcryPubkeySpec *) pubkey->spec)->nsig;
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  return nsig;
}

/****************
 * Return the number of encryption material numbers
 */
static int
pubkey_get_nenc (int id)
{
  GcryModule *pubkey;
  int nenc = 0;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_id (id);
  if (pubkey)
    {
      nenc = ((GcryPubkeySpec *) pubkey->spec)->nenc;
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  return nenc;
}


static int
pubkey_generate (int id, unsigned int nbits, unsigned long use_e,
                 MPI *skey, MPI **retfactors)
{
  GcryModule *pubkey;
  int err = GCRYERR_INV_PK_ALGO;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_id (id);
  if (pubkey)
    {
      err = (*((GcryPubkeySpec *) pubkey->spec)->generate) (id, nbits, use_e, skey,
							    retfactors);
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  return err;
}

static int
pubkey_check_secret_key (int id, MPI *skey)
{
  GcryModule *pubkey;
  int err = GCRYERR_INV_PK_ALGO;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  pubkey = gcry_pubkey_lookup_id (id);
  if (pubkey)
    {
      err = (*((GcryPubkeySpec *) pubkey->spec)->check_secret_key) (id, skey);
      _gcry_module_release (pubkey);
    }
  ath_mutex_unlock (&pubkeys_registered_lock);

  return err;
}


/****************
 * This is the interface to the public key encryption.
 * Encrypt DATA with PKEY and put it into RESARR which
 * should be an array of MPIs of size PUBKEY_MAX_NENC (or less if the
 * algorithm allows this - check with pubkey_get_nenc() )
 */
static int
pubkey_encrypt (int id, MPI *resarr, MPI data, MPI *pkey,
		int flags)
{
  GcryPubkeySpec *pubkey;
  GcryModule *module;
  int i, rc;

  if (DBG_CIPHER)
    {
      log_debug ("pubkey_encrypt: algo=%d\n", id);
      for(i = 0; i < pubkey_get_npkey (id); i++)
	log_mpidump ("  pkey:", pkey[i]);
      log_mpidump ("  data:", data);
    }

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  module = gcry_pubkey_lookup_id (id);
  if (module)
    {
      pubkey = (GcryPubkeySpec *) module->spec;
      rc = (*pubkey->encrypt) (id, resarr, data, pkey, flags);
      _gcry_module_release (module);
      goto ready;
    }
  rc = GCRYERR_INV_PK_ALGO;

 ready:
  ath_mutex_unlock (&pubkeys_registered_lock);

  if (!rc && DBG_CIPHER)
    {
      for(i = 0; i < pubkey_get_nenc (id); i++)
	log_mpidump("  encr:", resarr[i] );
    }
  return rc;
}


/****************
 * This is the interface to the public key decryption.
 * ALGO gives the algorithm to use and this implicitly determines
 * the size of the arrays.
 * result is a pointer to a mpi variable which will receive a
 * newly allocated mpi or NULL in case of an error.
 */
static int
pubkey_decrypt (int id, MPI *result, MPI *data, MPI *skey,
		int flags)
{
  GcryPubkeySpec *pubkey;
  GcryModule *module;
  int i, rc;

  *result = NULL; /* so the caller can always do a mpi_free */
  if (DBG_CIPHER)
    {
      log_debug ("pubkey_decrypt: algo=%d\n", id);
      for(i = 0; i < pubkey_get_nskey (id); i++)
	log_mpidump ("  skey:", skey[i]);
      for(i = 0; i < pubkey_get_nenc (id); i++)
	log_mpidump ("  data:", data[i]);
    }

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  module = gcry_pubkey_lookup_id (id);
  if (module)
    {
      pubkey = (GcryPubkeySpec *) module->spec;
      rc = (*pubkey->decrypt) (id, result, data, skey, flags);
      _gcry_module_release (module);
      goto ready;
    }

  rc = GCRYERR_INV_PK_ALGO;
  
 ready:
  ath_mutex_unlock (&pubkeys_registered_lock);

  if (! rc && DBG_CIPHER)
    log_mpidump (" plain:", *result);

  return rc;
}


/****************
 * This is the interface to the public key signing.
 * Sign data with skey and put the result into resarr which
 * should be an array of MPIs of size PUBKEY_MAX_NSIG (or less if the
 * algorithm allows this - check with pubkey_get_nsig() )
 */
static int
pubkey_sign (int id, MPI *resarr, MPI data, MPI *skey)
{
  GcryPubkeySpec *pubkey;
  GcryModule *module;
  int i, rc;

  if (DBG_CIPHER)
    {
      log_debug ("pubkey_sign: algo=%d\n", id);
      for(i = 0; i < pubkey_get_nskey (id); i++)
	log_mpidump ("  skey:", skey[i]);
      log_mpidump("  data:", data );
    }

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  module = gcry_pubkey_lookup_id (id);
  if (module)
    {
      pubkey = (GcryPubkeySpec *) module->spec;
      rc = (*pubkey->sign) (id, resarr, data, skey);
      _gcry_module_release (module);
      goto ready;
    }

  rc = GCRYERR_INV_PK_ALGO;

 ready:
  ath_mutex_unlock (&pubkeys_registered_lock);

  if (! rc && DBG_CIPHER)
    for (i = 0; i < pubkey_get_nsig (id); i++)
      log_mpidump ("   sig:", resarr[i]);

  return rc;
}

/****************
 * Verify a public key signature.
 * Return 0 if the signature is good
 */
static int
pubkey_verify (int id, MPI hash, MPI *data, MPI *pkey,
	       int (*cmp)(void *, MPI), void *opaquev)
{
  GcryPubkeySpec *pubkey;
  GcryModule *module;
  int i, rc;

  if (DBG_CIPHER)
    {
      log_debug ("pubkey_verify: algo=%d\n", id);
      for (i = 0; i < pubkey_get_npkey (id); i++)
	log_mpidump ("  pkey:", pkey[i]);
      for (i = 0; i < pubkey_get_nsig (id); i++)
	log_mpidump ("   sig:", data[i]);
      log_mpidump ("  hash:", hash);
    }

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  module = gcry_pubkey_lookup_id (id);
  if (module)
    {
      pubkey = (GcryPubkeySpec *) module->spec;
      rc = (*pubkey->verify) (id, hash, data, pkey, cmp, opaquev);
      _gcry_module_release (module);
      goto ready;
    }

  rc = GCRYERR_INV_PK_ALGO;

 ready:
  ath_mutex_unlock (&pubkeys_registered_lock);
  return rc;
}

/* Internal function.   */
static int
sexp_elements_extract (GCRY_SEXP key_sexp, const char *element_names,
		       GCRY_MPI *elements)
{
  int i, index, err = 0;
  const char *name;
  GCRY_SEXP list;

  for (name = element_names, index = 0; *name && (! err); name++, index++)
    {
      list = gcry_sexp_find_token (key_sexp, name, 1);
      if (! list)
	err = GCRYERR_NO_OBJ;
      else
	{
	  elements[index] = gcry_sexp_nth_mpi (list, 1, GCRYMPI_FMT_USG);
	  gcry_sexp_release (list);
	  if (! elements[index])
	    err = GCRYERR_INV_OBJ;
	}
    }

  if (err)
    for (i = 0; i < index; i++)
      if (elements[i])
	gcry_free (elements[i]);

  return err;
}

/****************
 * Convert a S-Exp with either a private or a public key to our
 * internal format. Currently we do only support the following
 * algorithms:
 *    dsa
 *    rsa
 *    openpgp-dsa
 *    openpgp-rsa
 *    openpgp-elg
 *    openpgp-elg-sig
 * Provide a SE with the first element be either "private-key" or
 * or "public-key". the followed by a list with its first element
 * be one of the above algorithm identifiers and the following
 * elements are pairs with parameter-id and value.
 * NOTE: we look through the list to find a list beginning with
 * "private-key" or "public-key" - the first one found is used.
 *
 * FIXME: Allow for encrypted secret keys here.
 *
 * Returns: A pointer to an allocated array of MPIs if the return value is
 *	    zero; the caller has to release this array.
 *
 * Example of a DSA public key:
 *  (private-key
 *    (dsa
 *	(p <mpi>)
 *	(g <mpi>)
 *	(y <mpi>)
 *	(x <mpi>)
 *    )
 *  )
 * The <mpi> are expected to be in GCRYMPI_FMT_USG
 */
static int
sexp_to_key( GCRY_SEXP sexp, int want_private, MPI **retarray,
             int *retalgo, int *r_algotblidx)
{
    GCRY_SEXP list, l2;
    const char *name;
    const char *s;
    size_t n;
    int i;
    int algo;
    const char *elems1, *elems2;
    GCRY_MPI *array;
    int err = 0;

    /* check that the first element is valid */
    list = gcry_sexp_find_token( sexp, want_private? "private-key"
						    :"public-key", 0 );
    if( !list )
	return GCRYERR_INV_OBJ; /* Does not contain a public- or private-key object */
    l2 = gcry_sexp_cadr( list );
    gcry_sexp_release ( list );
    list = l2;
    name = gcry_sexp_nth_data( list, 0, &n );
    if( !name ) {
	gcry_sexp_release ( list );
	return GCRYERR_INV_OBJ; /* invalid structure of object */
    }
    for(i=0; (s=algo_info_table[i].name); i++ ) {
	if( strlen(s) == n && !memcmp( s, name, n ) )
	    break;
    }
    if( !s ) {
	gcry_sexp_release ( list );
	return GCRYERR_INV_PK_ALGO; /* unknown algorithm */
    }
    if (r_algotblidx)
      *r_algotblidx = i;
    algo = algo_info_table[i].algo;
    elems1 = algo_info_table[i].common_elements;
    elems2 = want_private? algo_info_table[i].secret_elements
			 : algo_info_table[i].public_elements;
    array = gcry_calloc( strlen(elems1)+strlen(elems2)+1, sizeof *array );
    if( !array ) {
	gcry_sexp_release ( list );
	return GCRYERR_NO_MEM;
    }

    err = sexp_elements_extract (list, elems1, array);
    if (! err)
      err = sexp_elements_extract (list, elems2, array + strlen (elems1));
    
    gcry_sexp_release (list);

    if (err)
      gcry_free (array);
    else
      {
	*retarray = array;
	*retalgo = algo;
      }

    return err;
}

static int
sexp_to_sig( GCRY_SEXP sexp, MPI **retarray, int *retalgo)
{
    GCRY_SEXP list, l2;
    const char *name;
    const char *s;
    size_t n;
    int i;
    int algo;
    const char *elems;
    GCRY_MPI *array;
    int err = 0;

    /* check that the first element is valid */
    list = gcry_sexp_find_token( sexp, "sig-val" , 0 );
    if( !list )
	return GCRYERR_INV_OBJ; /* Does not contain a signature value object */
    l2 = gcry_sexp_cadr( list );
    gcry_sexp_release ( list );
    list = l2;
    if( !list )
	return GCRYERR_NO_OBJ; /* no cadr for the sig object */
    name = gcry_sexp_nth_data( list, 0, &n );
    if( !name ) {
	gcry_sexp_release ( list );
	return GCRYERR_INV_OBJ; /* invalid structure of object */
    }
    for(i=0; (s=sig_info_table[i].name); i++ ) {
	if( strlen(s) == n && !memcmp( s, name, n ) )
	    break;
    }
    if( !s ) {
	gcry_sexp_release ( list );
	return GCRYERR_INV_PK_ALGO; /* unknown algorithm */
    }
    algo = sig_info_table[i].algo;
    elems = sig_info_table[i].elements;
    array = gcry_calloc( (strlen(elems)+1) , sizeof *array );
    if( !array ) {
	gcry_sexp_release ( list );
	return GCRYERR_NO_MEM;
    }

    err = sexp_elements_extract (list, elems, array);
    gcry_sexp_release (list);

    if (err)
      gcry_free (array);
    else
      {
	*retarray = array;
	*retalgo = algo;
      }

    return err;
}


/****************
 * Take sexp and return an array of MPI as used for our internal decrypt
 * function.
 * s_data = (enc-val
 *           [(flags [pkcs1])
 *	      (<algo>
 *		(<param_name1> <mpi>)
 *		...
 *		(<param_namen> <mpi>)
 *	      ))
 * RET_MODERN is set to true when at least an empty flags list has been found.
 */
static int
sexp_to_enc( GCRY_SEXP sexp, MPI **retarray, int *retalgo,
             int *ret_modern, int *ret_want_pkcs1,
	     int *flags)
{
    GCRY_SEXP list, l2;
    const char *name;
    const char *s;
    size_t n;
    int i;
    int algo;
    int parsed_flags = 0;
    const char *elems;
    GCRY_MPI *array;
    int err = 0;

    *ret_want_pkcs1 = 0;
    *ret_modern = 0;
    /* check that the first element is valid */
    list = gcry_sexp_find_token( sexp, "enc-val" , 0 );
    if( !list )
	return GCRYERR_INV_OBJ; /* Does not contain a encrypted value object */
    l2 = gcry_sexp_nth (list, 1);
    if (!l2 ) {
	gcry_sexp_release (list);
	return GCRYERR_NO_OBJ; /* no cdr for the data object */
    }
    name = gcry_sexp_nth_data (l2, 0, &n);
    if (!name) {
	gcry_sexp_release (l2);
	gcry_sexp_release (list);
	return GCRYERR_INV_OBJ; /* invalid structure of object */
    }
    if ( n == 5 && !memcmp (name, "flags", 5)) {
      /* There is a flags element - process it */
      const char *s;

      *ret_modern = 1;
      for (i=gcry_sexp_length (l2)-1; i > 0; i--)
        {
          s = gcry_sexp_nth_data (l2, i, &n);
          if (!s)
            ; /* not a data element - ignore */
          else if ( n == 3 && !memcmp (s, "raw", 3))
            ; /* just a dummy because it is the default */
          else if ( n == 5 && !memcmp (s, "pkcs1", 5))
            *ret_want_pkcs1 = 1;
	  else if ( n == 11 && !memcmp (s, "no-blinding", 11))
	    parsed_flags |= PUBKEY_FLAG_NO_BLINDING;
          else
            {
              gcry_sexp_release (l2);
              gcry_sexp_release (list);
              return GCRYERR_INV_FLAG;
            }
        }
      
      /* Get the next which has the actual data */
      gcry_sexp_release (l2);
      l2 = gcry_sexp_nth (list, 2);
      if (!l2 ) {
	gcry_sexp_release (list);
	return GCRYERR_NO_OBJ; /* no cdr for the data object */
      }
      name = gcry_sexp_nth_data (l2, 0, &n);
      if (!name) {
	gcry_sexp_release (l2);
	gcry_sexp_release (list);
	return GCRYERR_INV_OBJ; /* invalid structure of object */
      }
    }
    gcry_sexp_release (list);
    list = l2; l2 = NULL;
    
    for(i=0; (s=enc_info_table[i].name); i++ ) {
	if( strlen(s) == n && !memcmp( s, name, n ) )
	    break;
    }
    if( !s ) {
	gcry_sexp_release (list);
	return GCRYERR_INV_PK_ALGO; /* unknown algorithm */
    }

    algo = enc_info_table[i].algo;
    elems = enc_info_table[i].elements;
    array = gcry_calloc( (strlen(elems)+1) , sizeof *array );
    if( !array ) {
	gcry_sexp_release ( list );
	return GCRYERR_NO_MEM;
    }

    err = sexp_elements_extract (list, elems, array);
    gcry_sexp_release (list);

    if (err)
      gcry_free (array);
    else
      {
	*retarray = array;
	*retalgo = algo;
	*flags = parsed_flags;
      }

    return err;
}

/* Take the hash value and convert into an MPI, suitable for for
   passing to the low level functions.  We currently support the
   old style way of passing just a MPI and the modern interface which
   allows to pass flags so that we can choose between raw and pkcs1
   padding - may be more padding options later. 

   (<mpi>)
   or
   (data
    [(flags [pkcs1])]
    [(hash <algo> <value>)]
    [(value <text>)]
   )
   
   Either the VALUE or the HASH element must be present for use
   with signatures.  VALUE is used for encryption.

   NBITS is the length of the key in bits. 

*/
static int 
sexp_data_to_mpi (GcrySexp input, unsigned int nbits, GcryMPI *ret_mpi,
                  int for_encryption, int *flags)
{
  int rc = 0;
  GcrySexp ldata, lhash, lvalue;
  int i;
  size_t n;
  const char *s;
  int is_raw = 0, is_pkcs1 = 0, unknown_flag=0; 
  int parsed_flags = 0, dummy_flags;

  if (! flags)
    flags = &dummy_flags;
  
  *ret_mpi = NULL;
  ldata = gcry_sexp_find_token (input, "data", 0);
  if (!ldata)
    { /* assume old style */
      *ret_mpi = gcry_sexp_nth_mpi (input, 0, 0);
      return *ret_mpi? 0 : GCRYERR_INV_OBJ;
    }

  /* see whether there is a flags object */
  {
    GcrySexp lflags = gcry_sexp_find_token (ldata, "flags", 0);
    if (lflags)
      { /* parse the flags list. */
        for (i=gcry_sexp_length (lflags)-1; i > 0; i--)
          {
            s = gcry_sexp_nth_data (lflags, i, &n);
            if (!s)
              ; /* not a data element*/
            else if ( n == 3 && !memcmp (s, "raw", 3))
              is_raw = 1;
            else if ( n == 5 && !memcmp (s, "pkcs1", 5))
              is_pkcs1 = 1;
	    else if (n == 11 && ! memcmp (s, "no-blinding", 11))
	      parsed_flags |= PUBKEY_FLAG_NO_BLINDING;
            else
              unknown_flag = 1;
          }
        gcry_sexp_release (lflags);
      }
  }

  if (!is_pkcs1 && !is_raw)
    is_raw = 1; /* default to raw */

  /* Get HASH or MPI */
  lhash = gcry_sexp_find_token (ldata, "hash", 0);
  lvalue = lhash? NULL : gcry_sexp_find_token (ldata, "value", 0);

  if (!(!lhash ^ !lvalue))
    rc = GCRYERR_INV_OBJ; /* none or both given */
  else if (unknown_flag)
    rc = GCRYERR_INV_FLAG;
  else if (is_raw && is_pkcs1 && !for_encryption)
    rc = GCRYERR_CONFLICT;
  else if (is_raw && lvalue)
    {
      *ret_mpi = gcry_sexp_nth_mpi (lvalue, 1, 0);
      if (!*ret_mpi)
        rc = GCRYERR_INV_OBJ;
    }
  else if (is_pkcs1 && lvalue && for_encryption)
    { /* create pkcs#1 block type 2 padding */
      unsigned char *frame = NULL;
      size_t nframe = (nbits+7) / 8;
      const void * value;
      size_t valuelen;
      unsigned char *p;

      if ( !(value=gcry_sexp_nth_data (lvalue, 1, &valuelen)) || !valuelen )
        rc = GCRYERR_INV_OBJ;
      else if (valuelen + 7 > nframe || !nframe)
        {
          /* Can't encode a VALUELEN value in a NFRAME bytes frame. */
          rc = GCRYERR_TOO_SHORT; /* the key is too short */
        }
      else if ( !(frame = gcry_malloc_secure (nframe)))
        rc = GCRYERR_NO_MEM;
      else
        {
          n = 0;
          frame[n++] = 0;
          frame[n++] = 2; /* block type */
          i = nframe - 3 - valuelen;
          assert (i > 0);
          p = gcry_random_bytes_secure (i, GCRY_STRONG_RANDOM);
          /* replace zero bytes by new values*/
          for (;;)
            {
              int j, k;
              unsigned char *pp;
              
              /* count the zero bytes */
              for (j=k=0; j < i; j++)
                {
                  if (!p[j])
                    k++;
                }
              if (!k)
                break; /* okay: no (more) zero bytes */
              
              k += k/128; /* better get some more */
              pp = gcry_random_bytes_secure (k, GCRY_STRONG_RANDOM);
              for (j=0; j < i && k; j++)
                {
                  if (!p[j])
                    p[j] = pp[--k];
                }
              gcry_free (pp);
            }
          memcpy (frame+n, p, i);
          n += i;
          gcry_free (p);
          
          frame[n++] = 0;
          memcpy (frame+n, value, valuelen);
          n += valuelen;
          assert (n == nframe);

          gcry_mpi_scan (ret_mpi, GCRYMPI_FMT_USG, frame, &nframe);
        }

      gcry_free(frame);
    }
  else if (is_pkcs1 && lhash && !for_encryption)
    { /* create pkcs#1 block type 1 padding */
      if (gcry_sexp_length (lhash) != 3)
        rc = GCRYERR_INV_OBJ;
      else if ( !(s=gcry_sexp_nth_data (lhash, 1, &n)) || !n )
        rc = GCRYERR_INV_OBJ;
      else
        {
          static struct { const char *name; int algo; } hashnames[] = 
          { { "sha1",   GCRY_MD_SHA1 },
            { "md5",    GCRY_MD_MD5 },
            { "rmd160", GCRY_MD_RMD160 },
            { "sha256", GCRY_MD_SHA256 },
            { "sha384", GCRY_MD_SHA384 },
            { "sha512", GCRY_MD_SHA512 },
            { "md2",    GCRY_MD_MD2 },
            { "md4",    GCRY_MD_MD4 },
            { "tiger",  GCRY_MD_TIGER },
            { "haval",  GCRY_MD_HAVAL },
            { NULL }
          };
          int algo;
          byte asn[100];
          byte *frame = NULL;
          size_t nframe = (nbits+7) / 8;
          const void * value;
          size_t valuelen;
          size_t asnlen, dlen;
            
          for (i=0; hashnames[i].name; i++)
            {
              if ( strlen (hashnames[i].name) == n
                   && !memcmp (hashnames[i].name, s, n))
                break;
            }

          algo = hashnames[i].algo;
          asnlen = DIM(asn);
          dlen = gcry_md_get_algo_dlen (algo);

          if (!hashnames[i].name)
            rc = GCRYERR_INV_MD_ALGO;
          else if ( !(value=gcry_sexp_nth_data (lhash, 2, &valuelen))
                    || !valuelen )
            rc = GCRYERR_INV_OBJ;
          else if (gcry_md_algo_info (algo, GCRYCTL_GET_ASNOID, asn, &asnlen))
            rc = GCRYERR_NOT_IMPL; /* we don't have all of the above algos */
          else if ( valuelen != dlen )
            {
              /* hash value does not match the length of digest for
                 the given algo */
              rc = GCRYERR_CONFLICT;
            }
          else if( !dlen || dlen + asnlen + 4 > nframe)
            {
              /* can't encode an DLEN byte digest MD into a NFRAME byte frame */
              rc = GCRYERR_TOO_SHORT;
            }
          else if ( !(frame = gcry_malloc (nframe)) )
            rc = GCRYERR_NO_MEM;
          else
            { /* assemble the pkcs#1 block type 1 */
              n = 0;
              frame[n++] = 0;
              frame[n++] = 1; /* block type */
              i = nframe - valuelen - asnlen - 3 ;
              assert (i > 1);
              memset (frame+n, 0xff, i );
              n += i;
              frame[n++] = 0;
              memcpy (frame+n, asn, asnlen);
              n += asnlen;
              memcpy (frame+n, value, valuelen );
              n += valuelen;
              assert (n == nframe);
      
              /* convert it into an MPI */
              gcry_mpi_scan (ret_mpi, GCRYMPI_FMT_USG, frame, &nframe);
            }
          
          gcry_free (frame);
        }
    }
  else
    rc = GCRYERR_CONFLICT;
   
  gcry_sexp_release (ldata);
  gcry_sexp_release (lhash);
  gcry_sexp_release (lvalue);

  if (! rc)
    *flags = parsed_flags;

  return rc;
}


/*
   Do a PK encrypt operation
  
   Caller has to provide a public key as the SEXP pkey and data as a
   SEXP with just one MPI in it. Alternativly S_DATA might be a
   complex S-Expression, similar to the one used for signature
   verification.  This provides a flag which allows to handle PKCS#1
   block type 2 padding.  The function returns a a sexp which may be
   passed to to pk_decrypt.
  
   Returns: 0 or an errorcode.
  
   s_data = See comment for sexp_data_to_mpi
   s_pkey = <key-as-defined-in-sexp_to_key>
   r_ciph = (enc-val
               (<algo>
                 (<param_name1> <mpi>)
                 ...
                 (<param_namen> <mpi>)
               ))

*/
int
gcry_pk_encrypt (GCRY_SEXP *r_ciph, GCRY_SEXP s_data, GCRY_SEXP s_pkey)
{
    MPI *pkey, data, *ciph;
    const char *key_algo_name, *algo_name, *algo_elems;
    int i, rc, algo, flags;
    
    *r_ciph = NULL;
    /* get the key */
    rc = sexp_to_key( s_pkey, 0, &pkey, &algo, &i);
    if( rc ) 
	return rc;
    key_algo_name = algo_info_table[i].name;
    assert (key_algo_name);

    /* get the name and the required size of the return value */
    for(i=0; (algo_name = enc_info_table[i].name); i++ ) {
	if( enc_info_table[i].algo == algo )
	    break;
    }
    /* get the name and the required size of the result array.  We
       compare using the algorithm name and not the algo number - this way
       we get the correct name for the return value */
    for(i=0; (algo_name = enc_info_table[i].name); i++ ) {
	if( !strcmp (algo_name, key_algo_name) )
	    break;
    }
    if( !algo_name ) {
	release_mpi_array( pkey );
        gcry_free (pkey);
	return GCRYERR_INV_PK_ALGO;
    }
    algo_elems = enc_info_table[i].elements;

    /* get the stuff we want to encrypt */
    rc = sexp_data_to_mpi (s_data, gcry_pk_get_nbits (s_pkey), &data, 1,
			   &flags);
    if (rc) {
	release_mpi_array( pkey );
        gcry_free (pkey);
	return GCRYERR_INV_OBJ;
    }

    /* Now we can encrypt data to ciph */
    ciph = gcry_xcalloc( (strlen(algo_elems)+1) , sizeof *ciph );
    rc = pubkey_encrypt( algo, ciph, data, pkey, flags );
    release_mpi_array( pkey );
    gcry_free (pkey); pkey = NULL;
    mpi_free( data );
    if( rc ) {
	release_mpi_array( ciph );
	gcry_free( ciph );
	return rc;
    }

    /* We did it.  Now build the return list */
    {
	char *string, *p;
	size_t nelem, needed= strlen(algo_name) + 30;

	/* FIXME, this calculation needs to be cleaned up.
	   -moritz  */

	/* count elements, so that we can allocate enough space */
	for(nelem=0; algo_elems[nelem]; nelem++ )
	    needed += 10; /* 6 + a safety margin */
	/* build the string */
	string = p = gcry_xmalloc ( needed );
	p = stpcpy ( p, "(enc-val(flags " );
	if (flags & PUBKEY_FLAG_NO_BLINDING)
	  p = stpcpy (p, "no-blinding");
	p = stpcpy (p, ")(");
	p = stpcpy ( p, algo_name );
	for(i=0; algo_elems[i]; i++ ) {
	    *p++ = '(';
	    *p++ = algo_elems[i];
	    p = stpcpy ( p, "%m)" );
	}
	strcpy ( p, "))" );
	/* and now the ugly part:  we don't have a function to
	 * pass an array to a format string, so we have to do it this way :-(
	 */
	switch ( nelem ) {
	  case 1: rc = gcry_sexp_build ( r_ciph, NULL, string,
		     ciph[0]
		  ); break;
	  case 2: rc = gcry_sexp_build ( r_ciph, NULL, string,
		     ciph[0], ciph[1]
		  ); break;
	  case 3: rc = gcry_sexp_build ( r_ciph, NULL, string,
		     ciph[0], ciph[1], ciph[2]
		  ); break;
	  case 4: rc = gcry_sexp_build ( r_ciph, NULL, string,
		     ciph[0], ciph[1], ciph[2], ciph[3]
		  ); break;
	  case 5: rc = gcry_sexp_build ( r_ciph, NULL, string,
		     ciph[0], ciph[1], ciph[2], ciph[3], ciph[4]
		  ); break;
	  case 6: rc = gcry_sexp_build ( r_ciph, NULL, string,
		     ciph[0], ciph[1], ciph[2], ciph[3], ciph[4], ciph[5]
		  ); break;
	  default: BUG ();
	}
	if ( rc )
	    BUG ();
	gcry_free ( string );
    }
    release_mpi_array( ciph );
    gcry_free( ciph );


    return 0;
}

/****************
 * Do a PK decrypt operation
 *
 * Caller has to provide a secret key as the SEXP skey and data in a
 * format as created by gcry_pk_encrypt.  For historic reasons the
 * function returns simply an MPI as an S-expression part; this is
 * deprecated and the new method should be used which returns a real
 * S-expressionl this is selected by adding at least an empt flags
 * list to S_DATA.
 * 
 * Returns: 0 or an errorcode.
 *
 * s_data = (enc-val
 *            [(flags)]
 *	      (<algo>
 *		(<param_name1> <mpi>)
 *		...
 *		(<param_namen> <mpi>)
 *	      ))
 * s_skey = <key-as-defined-in-sexp_to_key>
 * r_plain= Either an incomplete S-expression without the parentheses
 *          or if the flags list is used (even if empty) a real S-expression:
 *          (value PLAIN).  */
int
gcry_pk_decrypt( GCRY_SEXP *r_plain, GCRY_SEXP s_data, GCRY_SEXP s_skey )
{
    MPI *skey, *data, plain;
    int rc, algo, dataalgo, modern, want_pkcs1, flags;
    
    *r_plain = NULL;
    rc = sexp_to_key( s_skey, 1, &skey, &algo, NULL );
    if( rc ) {
	return rc;
    }
    rc = sexp_to_enc( s_data, &data, &dataalgo, &modern, &want_pkcs1,
		      &flags );
    if( rc ) {
	release_mpi_array( skey );
        gcry_free (skey);
	return rc;
    }
    if( algo != dataalgo ) {
	release_mpi_array( skey );
        gcry_free (skey);
	release_mpi_array( data );
        gcry_free (data);
	return GCRYERR_CONFLICT; /* key algo does not match data algo */
    }

    rc = pubkey_decrypt( algo, &plain, data, skey, flags );
    if( rc ) {
	release_mpi_array( skey );
        gcry_free (skey);
	release_mpi_array( data );
        gcry_free (data);
	return GCRYERR_GENERAL; /* decryption failed */
    }

    if (!modern) {
      if ( gcry_sexp_build( r_plain, NULL, "%m", plain ) )
	BUG ();
    }
    else {
      if ( gcry_sexp_build( r_plain, NULL, "(value %m)", plain ) )
	BUG ();
    }
      

    mpi_free( plain );
    release_mpi_array( data );
    gcry_free (data);
    release_mpi_array( skey );
    gcry_free (skey);
    return 0;
}



/****************
 * Create a signature.
 *
 * Caller has to provide a secret key as the SEXP skey and data
 * expressed as a SEXP list hash with only one element which should
 * instantly be available as a MPI. Alternatively the structure given
 * below may be used for S_HASH, it provides the abiliy to pass flags
 * to the operation; the only flag defined by now is "pkcs1" which
 * does PKCS#1 block type 1 style padding.
 *
 * Returns: 0 or an errorcode.
 *	    In case of 0 the function returns a new SEXP with the
 *	    signature value; the structure of this signature depends on the
 *	    other arguments but is always suitable to be passed to
 *	    gcry_pk_verify
 *
 * s_hash = See comment for sexp_data_to_mpi
 *             
 * s_skey = <key-as-defined-in-sexp_to_key>
 * r_sig  = (sig-val
 *	      (<algo>
 *		(<param_name1> <mpi>)
 *		...
 *		(<param_namen> <mpi>)
 * )) */
int
gcry_pk_sign( GCRY_SEXP *r_sig, GCRY_SEXP s_hash, GCRY_SEXP s_skey )
{
    MPI *skey, hash;
    MPI *result;
    int i, algo, rc;
    const char *key_algo_name, *algo_name, *algo_elems;

    *r_sig = NULL;
    rc = sexp_to_key( s_skey, 1, &skey, &algo, &i);
    if( rc )
	return rc;
    key_algo_name = algo_info_table[i].name;
    assert (key_algo_name);

    /* get the name and the required size of the result array.  We
       compare using the algorithm name and not the algo number - this way
       we get the correct name for the return value */
    for(i=0; (algo_name = sig_info_table[i].name); i++ ) {
	if( !strcmp (algo_name, key_algo_name) )
	    break;
    }
    if( !algo_name ) {
	release_mpi_array( skey );
        gcry_free (skey);
	return -4; /* oops: unknown algorithm */
    }
    assert (sig_info_table[i].algo == algo);
    algo_elems = sig_info_table[i].elements;

    /* get the stuff we want to sign */
    /* Note that pk_get_nbits does also work on a private key */
    rc = sexp_data_to_mpi (s_hash, gcry_pk_get_nbits (s_skey), &hash, 0, NULL);
    if (rc) {
	release_mpi_array( skey );
        gcry_free (skey);
	return rc; 
    }
    result = gcry_xcalloc( (strlen(algo_elems)+1) , sizeof *result );
    rc = pubkey_sign( algo, result, hash, skey );
    release_mpi_array( skey );
    gcry_free (skey); skey = NULL;
    mpi_free( hash );
    if( rc ) {
	gcry_free( result );
	return rc;
    }

    {
	char *string, *p;
	size_t nelem, needed= strlen(algo_name) + 20;

	/* count elements, so that we can allocate enough space */
	for(nelem=0; algo_elems[nelem]; nelem++ )
	    needed += 10; /* 6 + a safety margin */
	/* build the string */
	string = p = gcry_xmalloc ( needed );
	p = stpcpy ( p, "(sig-val(" );
	p = stpcpy ( p, algo_name );
	for(i=0; algo_elems[i]; i++ ) {
	    *p++ = '(';
	    *p++ = algo_elems[i];
	    p = stpcpy ( p, "%m)" );
	}
	strcpy ( p, "))" );
	/* and now the ugly part:  we don't have a function to
	 * pass an array to a format string, so we have to do it this way :-(
	 */
	switch ( nelem ) {
	  case 1: rc = gcry_sexp_build ( r_sig, NULL, string,
		     result[0]
		  ); break;
	  case 2: rc = gcry_sexp_build ( r_sig, NULL, string,
		     result[0], result[1]
		  ); break;
	  case 3: rc = gcry_sexp_build ( r_sig, NULL, string,
		     result[0], result[1], result[2]
		  ); break;
	  case 4: rc = gcry_sexp_build ( r_sig, NULL, string,
		     result[0], result[1], result[2], result[3]
		  ); break;
	  case 5: rc = gcry_sexp_build ( r_sig, NULL, string,
		     result[0], result[1], result[2], result[3], result[4]
		  ); break;
	  case 6: rc = gcry_sexp_build ( r_sig, NULL, string,
		     result[0], result[1], result[2], result[3], result[4], result[5]
		  ); break;
	  default: BUG ();
	}
	if ( rc )
	    BUG ();
	gcry_free ( string );
    }
    release_mpi_array( result );
    gcry_free( result );

    return 0;
}


/****************
 * Verify a sgnature.  Caller has to supply the public key pkey,
 * the signature sig and his hashvalue data.  Public key has to be
 * a standard public key given as an S-Exp, sig is a S-Exp as returned
 * from gcry_pk_sign and data must be an S-Exp like the one in sign too.
 */
int
gcry_pk_verify( GCRY_SEXP s_sig, GCRY_SEXP s_hash, GCRY_SEXP s_pkey )
{
    MPI *pkey, hash, *sig;
    int algo, sigalgo;
    int rc;

    rc = sexp_to_key( s_pkey, 0, &pkey, &algo, NULL );
    if( rc )
	return rc;
    rc = sexp_to_sig( s_sig, &sig, &sigalgo );
    if( rc ) {
	release_mpi_array( pkey );
        gcry_free (pkey);
	return rc;
    }
    if( algo != sigalgo ) {
	release_mpi_array( pkey );
        gcry_free (pkey);
	release_mpi_array( sig );
        gcry_free (sig);
	return GCRYERR_CONFLICT; /* algo does not match */
    }

    rc = sexp_data_to_mpi (s_hash, gcry_pk_get_nbits (s_pkey), &hash, 0, 0);
    if (rc) {
	release_mpi_array( pkey );
        gcry_free (pkey);
	release_mpi_array( sig );
        gcry_free (sig);
	return rc; 
    }

    rc = pubkey_verify( algo, hash, sig, pkey, NULL, NULL );
    release_mpi_array( pkey );
    gcry_free (pkey);
    release_mpi_array( sig );
    gcry_free (sig);
    mpi_free(hash);

    return rc;
}


/****************
 * Test a key.	This may be used either for a public or a secret key
 * to see whether internal structre is valid.
 *
 * Returns: 0 or an errorcode.
 *
 * s_key = <key-as-defined-in-sexp_to_key>
 */
int
gcry_pk_testkey( GCRY_SEXP s_key )
{
    MPI *key;
    int rc, algo;

    /* Note we currently support only secret key checking */
    rc = sexp_to_key( s_key, 1, &key, &algo, NULL );
    if( rc ) {
	return rc;
    }

    rc = pubkey_check_secret_key( algo, key );
    release_mpi_array( key );
    gcry_free (key);
    return rc;
}


/****************
 * Create a public key pair and return it in r_key.
 * How the key is created depends on s_parms:
 * (genkey
 *  (algo
 *    (parameter_name_1 ....)
 *     ....
 *    (parameter_name_n ....)
 * ))
 * The key is returned in a format depending on the
 * algorithm. Both, private and secret keys are returned
 * and optionally some additional informatin.
 * For elgamal we return this structure:
 * (key-data
 *  (public-key
 *    (elg
 *	(p <mpi>)
 *	(g <mpi>)
 *	(y <mpi>)
 *    )
 *  )
 *  (private-key
 *    (elg
 *	(p <mpi>)
 *	(g <mpi>)
 *	(y <mpi>)
 *	(x <mpi>)
 *    )
 *  )
 *  (misc-key-info
 *     (pm1-factors n1 n2 ... nn)
 *  )
 * )
 */
int
gcry_pk_genkey( GCRY_SEXP *r_key, GCRY_SEXP s_parms )
{
    GCRY_SEXP list, l2;
    const char *name;
    const char *s, *s2;
    size_t n;
    int rc, i;
    const char *algo_name;
    int algo;
    char sec_elems[20], pub_elems[20];
    GCRY_MPI skey[10], *factors;
    unsigned int nbits;
    unsigned long use_e;

    *r_key = NULL;
    list = gcry_sexp_find_token( s_parms, "genkey", 0 );
    if( !list )
	return GCRYERR_INV_OBJ; /* Does not contain genkey data */
    l2 = gcry_sexp_cadr( list );
    gcry_sexp_release ( list );
    list = l2;
    if( !list )
	return GCRYERR_NO_OBJ; /* no cdr for the genkey */
    name = gcry_sexp_nth_data( list, 0, &n );
    if( !name ) {
	gcry_sexp_release ( list );
	return GCRYERR_INV_OBJ; /* algo string missing */
    }
    for(i=0; (s=algo_info_table[i].name); i++ ) {
	if( strlen(s) == n && !memcmp( s, name, n ) )
	    break;
    }
    if( !s ) {
	gcry_sexp_release ( list );
	return GCRYERR_INV_PK_ALGO; /* unknown algorithm */
    }

    algo = algo_info_table[i].algo;
    algo_name = algo_info_table[i].name;
    
    s = algo_info_table[i].common_elements;
    s2 = algo_info_table[i].public_elements;
    if( strlen( s ) + strlen( s2 ) > DIM( pub_elems ) )
        return GCRYERR_INTERNAL; /* check bound failed */
    strcpy( pub_elems, s );
    strcat( pub_elems, s2 );

    s = algo_info_table[i].common_elements;
    s2 = algo_info_table[i].secret_elements;
    if( strlen( s ) + strlen( s2 ) > DIM( sec_elems ) )
        return GCRYERR_INTERNAL; /* check bound failed */
    strcpy( sec_elems, s );
    strcat( sec_elems, s2 );

    l2 = gcry_sexp_find_token (list, "rsa-use-e", 0);
    if (l2)
      {
        char buf[50];

        name = gcry_sexp_nth_data (l2, 1, &n);
        if (!name || n >= DIM (buf)-1 )
           {
             gcry_sexp_release (l2);
             gcry_sexp_release (list);
             return GCRYERR_INV_OBJ; /* no value or value too large */
           }
        
        memcpy (buf, name, n);
	buf[n] = 0;
	use_e = strtoul (buf, NULL, 0);
        gcry_sexp_release (l2);
      }
    else
      use_e = 65537; /* not given, use the value generated by old versions. */

    l2 = gcry_sexp_find_token( list, "nbits", 0 );
    gcry_sexp_release ( list );
    list = l2;
    if( !list )
	return GCRYERR_NO_OBJ; /* no nbits parameter */
    name = gcry_sexp_nth_data( list, 1, &n );
    if( !name ) {
	gcry_sexp_release ( list );
	return GCRYERR_INV_OBJ; /* nbits without a cdr */
    }
    {
	char *p = gcry_xmalloc(n+1);
	memcpy(p, name, n );
	p[n] = 0;
	nbits = (unsigned int)strtol( p, NULL, 0 );
	gcry_free( p );
    }
    gcry_sexp_release ( list );

    rc = pubkey_generate( algo, nbits, use_e, skey, &factors );
    if( rc ) {
	return rc;
    }

    {
	char *string, *p;
	size_t nelem=0, needed=0;
	GCRY_MPI mpis[30];


	/* count elements, so that we can allocate enough space */
	for(i=0; pub_elems[i]; i++, nelem++ )
	    needed += 10; /* 6 + a safety margin */
	for(i=0; sec_elems[i]; i++, nelem++ )
	    needed += 10; /* 6 + a safety margin */
	for(i=0; factors[i]; i++, nelem++ )
	    needed += 10; /* 6 + a safety margin */
	needed += 2* strlen(algo_name) +  300;
	if ( nelem > DIM(mpis) )
	    BUG ();

	/* build the string */
	nelem = 0;
	string = p = gcry_xmalloc ( needed );
	p = stpcpy ( p, "(key-data" );

	p = stpcpy ( p, "(public-key(" );
	p = stpcpy ( p, algo_name );
	for(i=0; pub_elems[i]; i++ ) {
	    *p++ = '(';
	    *p++ = pub_elems[i];
	    p = stpcpy ( p, "%m)" );
	    mpis[nelem++] = skey[i];
	}
	p = stpcpy ( p, "))" );

	p = stpcpy ( p, "(private-key(" );
	p = stpcpy ( p, algo_name );
	for(i=0; sec_elems[i]; i++ ) {
	    *p++ = '(';
	    *p++ = sec_elems[i];
	    p = stpcpy ( p, "%m)" );
	    mpis[nelem++] = skey[i];
	}
	p = stpcpy ( p, "))" );
	/* Very ugly hack to make release_mpi_array() work FIXME */
	skey[i] = NULL;

	p = stpcpy ( p, "(misc-key-info(pm1-factors" );
	for(i=0; factors[i]; i++ ) {
	    p = stpcpy ( p, "%m" );
	    mpis[nelem++] = factors[i];
	}
	strcpy ( p, ")))" );

	while ( nelem < DIM(mpis) )
	    mpis[nelem++] = NULL;

	/* and now the ugly part:  we don't have a function to
	 * pass an array to a format string, so we have just pass everything
	 * we have. which normally should be no problem as only those
	 * with a corresponding %m are used
	 */
	if ( gcry_sexp_build ( r_key, NULL, string,
		   mpis[0], mpis[1], mpis[2], mpis[3], mpis[4], mpis[5],
		   mpis[6], mpis[7], mpis[8], mpis[9], mpis[10], mpis[11],
		   mpis[12], mpis[13], mpis[14], mpis[15], mpis[16], mpis[17],
		   mpis[18], mpis[19], mpis[20], mpis[21], mpis[22], mpis[23],
		   mpis[24], mpis[25], mpis[26], mpis[27], mpis[28], mpis[29]
		  ) )
	    BUG ();
	assert ( DIM(mpis) == 30 );
	gcry_free ( string );
    }
    release_mpi_array ( skey );
    /* no free:  skey is a static array */
    release_mpi_array ( factors );
    gcry_free (factors);

    return 0;
}

/****************
 * Get the number of nbits from the public key
 * Hmmm: Should we have really this function or is it
 * better to have a more general function to retrieve
 * different propoerties of the key?
 */
unsigned int
gcry_pk_get_nbits (GCRY_SEXP key)
{
  GcryPubkeySpec *pubkey;
  GcryModule *module;
  int rc, id;
  MPI *keyarr;
  unsigned int nbits = 0;

  rc = sexp_to_key (key, 0, &keyarr, &id, NULL);
  if (rc == GCRYERR_INV_OBJ)
    rc = sexp_to_key (key, 1, &keyarr, &id, NULL);
  if (rc)
    return 0;

  REGISTER_DEFAULT_PUBKEYS;

  ath_mutex_lock (&pubkeys_registered_lock);
  module = gcry_pubkey_lookup_id (id);
  if (module)
    {
      pubkey = (GcryPubkeySpec *) module->spec;
      nbits = (*pubkey->get_nbits) (id, keyarr);
      _gcry_module_release (module);
      goto leave;
    }

  if (is_RSA (id))	/* we always wanna see the length of a key :-) */
    nbits = mpi_get_nbits (keyarr[0]);

 leave:
  ath_mutex_unlock (&pubkeys_registered_lock);

  release_mpi_array (keyarr);
  gcry_free (keyarr);
  return nbits;
}


/* Return the so called KEYGRIP which is the SHA-1 hash of the public
   key parameters expressed in a way depended on the algorithm.

   ARRAY must either be 20 bytes long or NULL; in the latter case a
   newly allocated array of that size is returned, otherwise ARRAY or
   NULL is returned to indicate an error which is most likely an
   unknown algorithm.  The function accepts public or secret keys. */
unsigned char *
gcry_pk_get_keygrip (GCRY_SEXP key, unsigned char *array)
{
  GCRY_SEXP list=NULL, l2;
  const char *s, *name;
  size_t n;
  int i, idx;
  int is_rsa;
  const char *elems;
  GCRY_MD_HD md = NULL;

  /* check that the first element is valid */
  list = gcry_sexp_find_token (key, "public-key", 0);
  if (!list)
    list = gcry_sexp_find_token (key, "private-key", 0);
  if (!list)
    list = gcry_sexp_find_token (key, "protected-private-key", 0);
  if (!list)
    return NULL; /* no public- or private-key object */

  l2 = gcry_sexp_cadr (list);
  gcry_sexp_release (list);
  list = l2;

  name = gcry_sexp_nth_data( list, 0, &n );
  if (!name)
    goto fail; /* invalid structure of object */

  for (i=0; (s=algo_info_table[i].name); i++ ) 
    {
      if (strlen(s) == n && !memcmp (s, name, n))
        break;
    }
  
  if(!s)
    goto fail; /* unknown algorithm */

  is_rsa = algo_info_table[i].algo == GCRY_PK_RSA;
  elems = algo_info_table[i].grip_elements;
  if (!elems)
    goto fail; /* no grip parameter */
    
  md = gcry_md_open (GCRY_MD_SHA1, 0);
  if (!md)
    goto fail;

  idx = 0;
  for (s=elems; *s; s++, idx++)
    {
      const char *data;
      size_t datalen;

      l2 = gcry_sexp_find_token (list, s, 1);
      if (!l2)
        goto fail;
      data = gcry_sexp_nth_data (l2, 1, &datalen);
      gcry_sexp_release (l2);
      if (!data)
        goto fail;
      if (!is_rsa)
        {
          char buf[30];

          sprintf (buf, "(1:%c%u:", *s, (unsigned int)datalen);
          gcry_md_write (md, buf, strlen (buf));
        }
      /* pkcs-15 says that for RSA only the modulus should be hashed -
         however, it is not clear wether this is meant to has the raw
         bytes assuming this is an unsigned integer or whether the DER
         required 0 should be prefixed. We hash th raw bytes.  For
         non-RSA we hash S-expressions. */
      gcry_md_write (md, data, datalen);
      if (!is_rsa)
        gcry_md_write (md, ")", 1);
    }
  
  if (!array)
    {
      array = gcry_malloc (20);
      if (!array)
        goto fail;
    }
  memcpy (array, gcry_md_read (md, GCRY_MD_SHA1), 20);
  gcry_md_close (md);
  gcry_sexp_release (list);
  return array;

 fail:
  gcry_md_close (md);
  gcry_sexp_release (list);
  return NULL;
}



int
gcry_pk_ctl( int cmd, void *buffer, size_t buflen)
{
    switch( cmd ) {
      case GCRYCTL_DISABLE_ALGO:
	/* this one expects a buffer pointing to an
	 * integer with the algo number.
	 */
	if( !buffer || buflen != sizeof(int) )
	    return set_lasterr( GCRYERR_INV_CIPHER_ALGO );
	disable_pubkey_algo( *(int*)buffer );
	break;

      default:
	return set_lasterr( GCRYERR_INV_OP );
    }
    return 0;
}


/****************
 * Return information about the given algorithm
 * WHAT select the kind of information returned:
 *  GCRYCTL_TEST_ALGO:
 *	Returns 0 when the specified algorithm is available for use.
 *	Buffer must be NULL, nbytes  may have the address of a variable
 *	with the required usage of the algorithm. It may be 0 for don't
 *	care or a combination of the GCRY_PK_USAGE_xxx flags;
 *  GCRYCTL_GET_ALGO_USAGE:
 *      Return the usage glafs for the give algo.  An invalid alog
 *      does return 0.  Disabled algos are ignored here becuase we
 *      only want to know whether the algo is at all capable of
 *      the usage.
 *
 * On error the value -1 is returned and the error reason may be
 * retrieved by gcry_errno().
 * Note:  Because this function is in most cases used to return an
 * integer value, we can make it easier for the caller to just look at
 * the return value.  The caller will in all cases consult the value
 * and thereby detecting whether a error occured or not (i.e. while checking
 * the block size)
 */
int
gcry_pk_algo_info (int id, int what, void *buffer, size_t *nbytes)
{
  switch (what)
    {
    case GCRYCTL_TEST_ALGO:
      {
	int use = nbytes ? *nbytes: 0;
	if (buffer)
	  {
	    set_lasterr( GCRYERR_INV_ARG );
	    return -1;
	  }
	if (check_pubkey_algo (id, use))
	  {
	    set_lasterr( GCRYERR_INV_PK_ALGO );
	    return -1;
	  }
      }
      break;

    case GCRYCTL_GET_ALGO_USAGE:
      {
	GcryModule *pubkey;
	int use = 0;

	REGISTER_DEFAULT_PUBKEYS;

	ath_mutex_lock (&pubkeys_registered_lock);
	pubkey = gcry_pubkey_lookup_id (id);
	if (pubkey)
	  {
	    use = ((GcryPubkeySpec *) pubkey->spec)->use;
	    _gcry_module_release (pubkey);
	  }
	ath_mutex_unlock (&pubkeys_registered_lock);
	return use;
      }

    case GCRYCTL_GET_ALGO_NPKEY:
      return pubkey_get_npkey (id);
    case GCRYCTL_GET_ALGO_NSKEY:
      return pubkey_get_nskey (id);
    case GCRYCTL_GET_ALGO_NSIGN:
      return pubkey_get_nsig (id);
    case GCRYCTL_GET_ALGO_NENCR:
      return pubkey_get_nenc (id);

    default:
      set_lasterr (GCRYERR_INV_OP);
      return -1;
    }

  return 0;
}
