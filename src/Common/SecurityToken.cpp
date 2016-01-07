/*
 Derived from source code of TrueCrypt 7.1a, which is
 Copyright (c) 2008-2012 TrueCrypt Developers Association and which is governed
 by the TrueCrypt License 3.0.

 Modifications and additions to the original source code (contained in this file) 
 and all other portions of this file are Copyright (c) 2013-2015 IDRIX
 and are governed by the Apache License 2.0 the full text of which is
 contained in the file License.txt included in VeraCrypt binary and source
 code distribution packages.
*/

#include "Platform/Finally.h"
#include "Platform/ForEach.h"

#if !defined (TC_WINDOWS) || defined (TC_PROTOTYPE)
#	include "Platform/SerializerFactory.h"
#	include "Platform/StringConverter.h"
#	include "Platform/SystemException.h"
#else
#	include "Dictionary.h"
#	include "Language.h"
#endif

 #include <Platform/File.h>
 #include <Platform/FilesystemPath.h>
 #include <Volume/Crc32.h>

#ifdef TC_UNIX
#	include <dlfcn.h>
#endif

#include "SecurityToken.h"

#ifndef burn
#	define burn Memory::Erase
#endif

using namespace std;

namespace VeraCrypt
{
	SecurityTokenKeyfile::SecurityTokenKeyfile (const SecurityTokenKeyfilePath &path)
	{
		wstring pathStr = path;
		unsigned long slotId;

		if (swscanf (pathStr.c_str(), TC_SECURITY_TOKEN_KEYFILE_URL_PREFIX TC_SECURITY_TOKEN_KEYFILE_URL_SLOT L"/%lu", &slotId) != 1)
			throw InvalidSecurityTokenKeyfilePath();

		SlotId = slotId;

		size_t keyIdPos = pathStr.find (L"/" TC_SECURITY_TOKEN_KEYFILE_URL_FILE L"/");
		if (keyIdPos == wstring::npos)
			throw InvalidSecurityTokenKeyfilePath();

		Id = pathStr.substr (keyIdPos + wstring (L"/" TC_SECURITY_TOKEN_KEYFILE_URL_FILE L"/").size());

		vector <SecurityTokenKeyfile> keyfiles = SecurityToken::GetAvailableKeyfiles (&SlotId, Id);

		if (keyfiles.empty())
			throw SecurityTokenKeyfileNotFound();

		*this = keyfiles.front();
	}

	SecurityTokenKeyfile::operator SecurityTokenKeyfilePath () const
	{
		wstringstream path;
		path << TC_SECURITY_TOKEN_KEYFILE_URL_PREFIX TC_SECURITY_TOKEN_KEYFILE_URL_SLOT L"/" << SlotId << L"/" TC_SECURITY_TOKEN_KEYFILE_URL_FILE L"/" << Id;
		return path.str();
	}

	void SecurityToken::CheckLibraryStatus ()
	{
		if (!Initialized)
			throw SecurityTokenLibraryNotInitialized();
	}

	void SecurityToken::CloseLibrary ()
	{
		if (Initialized)
		{
			CloseAllSessions();
			Pkcs11Functions->C_Finalize (NULL_PTR);

#ifdef TC_WINDOWS
			FreeLibrary (Pkcs11LibraryHandle);
#else
			dlclose (Pkcs11LibraryHandle);
#endif
			Initialized = false;
		}
	}

	void SecurityToken::CloseAllSessions () throw ()
	{
		if (!Initialized)
			return;

		typedef pair <CK_SLOT_ID, Pkcs11Session> SessionMapPair;

		foreach (SessionMapPair p, Sessions)
		{
			try
			{
				CloseSession (p.first);
			}
			catch (...) { }
		}
	}

	void SecurityToken::CloseSession (CK_SLOT_ID slotId)
	{
		if (Sessions.find (slotId) == Sessions.end())
			throw ParameterIncorrect (SRC_POS);

		Pkcs11Functions->C_CloseSession (Sessions[slotId].Handle);
		Sessions.erase (Sessions.find (slotId));
	}

	void SecurityToken::CreateKeyfile (CK_SLOT_ID slotId, vector <byte> &keyfileData, const string &name)
	{
		if (name.empty())
			throw ParameterIncorrect (SRC_POS);

		LoginUserIfRequired (slotId);

		foreach (const SecurityTokenKeyfile &keyfile, GetAvailableKeyfiles (&slotId))
		{
			if (keyfile.IdUtf8 == name)
				throw SecurityTokenKeyfileAlreadyExists();
		}

		CK_OBJECT_CLASS dataClass = CKO_DATA;
		CK_BBOOL trueVal = CK_TRUE;

		CK_ATTRIBUTE keyfileTemplate[] =
		{
			{ CKA_CLASS, &dataClass, sizeof (dataClass) },
			{ CKA_TOKEN, &trueVal, sizeof (trueVal) },
			{ CKA_PRIVATE, &trueVal, sizeof (trueVal) },
			{ CKA_LABEL, (CK_UTF8CHAR *) name.c_str(), (CK_ULONG) name.size() },
			{ CKA_VALUE, &keyfileData.front(), (CK_ULONG) keyfileData.size() }
		};

		CK_OBJECT_HANDLE keyfileHandle;

		CK_RV status = Pkcs11Functions->C_CreateObject (Sessions[slotId].Handle, keyfileTemplate, array_capacity (keyfileTemplate), &keyfileHandle);

		switch (status)
		{
		case CKR_DATA_LEN_RANGE:
			status = CKR_DEVICE_MEMORY;
			break;

		case CKR_SESSION_READ_ONLY:
			status = CKR_TOKEN_WRITE_PROTECTED;
			break;
		}

		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		// Some tokens report success even if the new object was truncated to fit in the available memory
		vector <byte> objectData;

		GetObjectAttribute (slotId, keyfileHandle, CKA_VALUE, objectData);
		finally_do_arg (vector <byte> *, &objectData, { if (!finally_arg->empty()) burn (&finally_arg->front(), finally_arg->size()); });

		if (objectData.size() != keyfileData.size())
		{
			Pkcs11Functions->C_DestroyObject (Sessions[slotId].Handle, keyfileHandle);
			throw Pkcs11Exception (CKR_DEVICE_MEMORY);
		}
	}

	void SecurityToken::DeleteKeyfile (const SecurityTokenKeyfile &keyfile)
	{
		LoginUserIfRequired (keyfile.SlotId);
		
		CK_RV status = Pkcs11Functions->C_DestroyObject (Sessions[keyfile.SlotId].Handle, keyfile.Handle);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);
	}


	void SecurityToken::GetSecurityTokenKey(wstring tokenKeyDescriptor, SecurityTokenKey &key)
	{

		CK_SLOT_ID slotId;

		if (swscanf (tokenKeyDescriptor.c_str(), L"%lu", &slotId) != 1)
			throw InvalidSecurityTokenKeyfilePath();

		size_t n = tokenKeyDescriptor.find(L":");
		if (n == std::string::npos) {
			throw InvalidSecurityTokenKeyfilePath();
		}
		
		wstring keyId = tokenKeyDescriptor.substr(n+1);
		trace_msgw(slotId);
		trace_msgw(keyId);
		

		vector <SecurityTokenKey> keys = SecurityToken::GetAvailableKeys(&slotId, keyId);
		if (keys.size() > 1 || keys.size() == 0) {
			throw Pkcs11Exception (CKR_TOKEN_NOT_RECOGNIZED);
		}
		key = keys[0];
	}

	vector <SecurityTokenKey> SecurityToken::GetAvailableKeys(CK_SLOT_ID *slotIdFilter, const wstring keyIdFilter)
	{
		bool unrecognizedTokenPresent = false;
		vector <SecurityTokenKey> keys;

		foreach (const CK_SLOT_ID &slotId, GetTokenSlots())
		{
			trace_msg("got next slot");
			SecurityTokenInfo token;

			if (slotIdFilter && *slotIdFilter != slotId)
				continue;

			try
			{
				trace_msg("logging in");
				LoginUserIfRequired (slotId);
				trace_msg("getting token info for slot");
				token = GetTokenInfo (slotId);
			}
			catch (UserAbort &)
			{
				continue;
			}
			catch (Pkcs11Exception &e)
			{
				if (e.GetErrorCode() == CKR_TOKEN_NOT_RECOGNIZED)
				{
					unrecognizedTokenPresent = true;
					continue;
				}
				trace_msg("pkcs11 errored");

				throw;
			}

			trace_msg("getting private keys");
			foreach (const CK_OBJECT_HANDLE &dataHandle, GetObjects (slotId, CKO_PRIVATE_KEY))
			{
				SecurityTokenKey key;
				key.Handle = dataHandle;
				key.SlotId = slotId;
				key.Token = token;

				trace_msg("got private key");


				vector <byte> privateAttrib;
				GetObjectAttribute (slotId, dataHandle, CKA_PRIVATE, privateAttrib);

				if (privateAttrib.size() == sizeof (CK_BBOOL) && *(CK_BBOOL *) &privateAttrib.front() != CK_TRUE)
					continue;

				GetObjectAttribute (slotId, dataHandle, CKA_MODULUS_BITS, privateAttrib);
				if (privateAttrib.size() == sizeof (CK_ULONG)) {
					CK_ULONG modulus = *(CK_ULONG *) &privateAttrib.front();
					key.maxDecryptBufferSize = modulus / 8 - 11;
					key.maxEncryptBufferSize = modulus / 8;
				}


				// TODO: check if CKA_DECRYPT is present
				// TODO: check if it is RSA

				vector <byte> label;
				GetObjectAttribute (slotId, dataHandle, CKA_LABEL, label);
				label.push_back (0);

				key.IdUtf8 = (char *) &label.front();

#if defined (TC_WINDOWS) && !defined (TC_PROTOTYPE)
				key.Id = Utf8StringToWide ((const char *) &label.front());
#else
				key.Id = StringConverter::ToWide ((const char *) &label.front());
#endif
				trace_msgw(key.Id);

				if (key.Id.empty() || (!keyIdFilter.empty() && keyIdFilter != key.Id)) {
					trace_msg("private key doesn't match with filter");
					continue;
				}

				keys.push_back (key);

				if (!keyIdFilter.empty())
					break;
			}
		}

		if (keys.empty() && unrecognizedTokenPresent)
			throw Pkcs11Exception (CKR_TOKEN_NOT_RECOGNIZED);

		return keys;
	}

	vector <SecurityTokenKeyfile> SecurityToken::GetAvailableKeyfiles (CK_SLOT_ID *slotIdFilter, const wstring keyfileIdFilter)
	{
		bool unrecognizedTokenPresent = false;
		vector <SecurityTokenKeyfile> keyfiles;

		foreach (const CK_SLOT_ID &slotId, GetTokenSlots())
		{
			SecurityTokenInfo token;

			if (slotIdFilter && *slotIdFilter != slotId)
				continue;

			try
			{
				LoginUserIfRequired (slotId);
				token = GetTokenInfo (slotId);
			}
			catch (UserAbort &)
			{
				continue;
			}
			catch (Pkcs11Exception &e)
			{
				if (e.GetErrorCode() == CKR_TOKEN_NOT_RECOGNIZED)
				{
					unrecognizedTokenPresent = true;
					continue;
				}

				throw;
			}

			foreach (const CK_OBJECT_HANDLE &dataHandle, GetObjects (slotId, CKO_DATA))
			{
				SecurityTokenKeyfile keyfile;
				keyfile.Handle = dataHandle;
				keyfile.SlotId = slotId;
				keyfile.Token = token;

				vector <byte> privateAttrib;
				GetObjectAttribute (slotId, dataHandle, CKA_PRIVATE, privateAttrib);

				if (privateAttrib.size() == sizeof (CK_BBOOL) && *(CK_BBOOL *) &privateAttrib.front() != CK_TRUE)
					continue;

				vector <byte> label;
				GetObjectAttribute (slotId, dataHandle, CKA_LABEL, label);
				label.push_back (0);

				keyfile.IdUtf8 = (char *) &label.front();

#if defined (TC_WINDOWS) && !defined (TC_PROTOTYPE)
				keyfile.Id = Utf8StringToWide ((const char *) &label.front());
#else
				keyfile.Id = StringConverter::ToWide ((const char *) &label.front());
#endif
				if (keyfile.Id.empty() || (!keyfileIdFilter.empty() && keyfileIdFilter != keyfile.Id))
					continue;

				keyfiles.push_back (keyfile);

				if (!keyfileIdFilter.empty())
					break;
			}
		}

		if (keyfiles.empty() && unrecognizedTokenPresent)
			throw Pkcs11Exception (CKR_TOKEN_NOT_RECOGNIZED);

		return keyfiles;
	}

	list <SecurityTokenInfo> SecurityToken::GetAvailableTokens ()
	{
		bool unrecognizedTokenPresent = false;
		list <SecurityTokenInfo> tokens;

		foreach (const CK_SLOT_ID &slotId, GetTokenSlots())
		{
			try
			{
				tokens.push_back (GetTokenInfo (slotId));
			}
			catch (Pkcs11Exception &e)
			{
				if (e.GetErrorCode() == CKR_TOKEN_NOT_RECOGNIZED)
				{
					unrecognizedTokenPresent = true;
					continue;
				}

				throw;
			}
		}

		if (tokens.empty() && unrecognizedTokenPresent)
			throw Pkcs11Exception (CKR_TOKEN_NOT_RECOGNIZED);

		return tokens;
	}

	SecurityTokenInfo SecurityToken::GetTokenInfo (CK_SLOT_ID slotId)
	{
		trace_msg("Getting token info");
		CK_TOKEN_INFO info;
		CK_RV status = Pkcs11Functions->C_GetTokenInfo (slotId, &info);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		SecurityTokenInfo token;
		token.SlotId = slotId;
		token.Flags = info.flags;

		char label[sizeof (info.label) + 1];
		memset (label, 0, sizeof (label));
		memcpy (label, info.label, sizeof (info.label));

		token.LabelUtf8 = label;

		size_t lastSpace = token.LabelUtf8.find_last_not_of (' ');
		if (lastSpace == string::npos)
			token.LabelUtf8.clear();
		else
			token.LabelUtf8 = token.LabelUtf8.substr (0, lastSpace + 1);

#if defined (TC_WINDOWS) && !defined (TC_PROTOTYPE)
		token.Label = Utf8StringToWide (token.LabelUtf8);
#else
		token.Label = StringConverter::ToWide (token.LabelUtf8);
#endif
		return token;
	}

	void SecurityToken::GetKeyfileData (const SecurityTokenKeyfile &keyfile, vector <byte> &keyfileData)
	{
		LoginUserIfRequired (keyfile.SlotId);
		GetObjectAttribute (keyfile.SlotId, keyfile.Handle, CKA_VALUE, keyfileData);
	}

	vector <CK_OBJECT_HANDLE> SecurityToken::GetObjects (CK_SLOT_ID slotId, CK_ATTRIBUTE_TYPE objectClass)
	{
		if (Sessions.find (slotId) == Sessions.end())
			throw ParameterIncorrect (SRC_POS);

		CK_ATTRIBUTE findTemplate;
		findTemplate.type = CKA_CLASS;
		findTemplate.pValue = &objectClass;
		findTemplate.ulValueLen = sizeof (objectClass);

		CK_RV status = Pkcs11Functions->C_FindObjectsInit (Sessions[slotId].Handle, &findTemplate, 1);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		finally_do_arg (CK_SLOT_ID, slotId, { Pkcs11Functions->C_FindObjectsFinal (Sessions[finally_arg].Handle); });

		CK_ULONG objectCount;	
		vector <CK_OBJECT_HANDLE> objects;

		while (true)
		{
			CK_OBJECT_HANDLE object;
			CK_RV status = Pkcs11Functions->C_FindObjects (Sessions[slotId].Handle, &object, 1, &objectCount);
			if (status != CKR_OK)
				throw Pkcs11Exception (status);

			if (objectCount != 1)
				break;

			objects.push_back (object);
		}

		return objects;
	}

	CK_RV SecurityToken::PKCS11Decrypt(
		CK_SESSION_HANDLE hSession,
		vector<byte> inEncryptedData,
		vector<byte> &outData
		)
	{
		CK_RV rv;
		if (!inEncryptedData.size())
			return CKR_ARGUMENTS_BAD;

		trace_msg("vectors to buffers");
		CK_ULONG ulInDataLen = 0;
		CK_BYTE* pInData = Vector2Buffer(inEncryptedData, ulInDataLen);
		CK_ULONG ulOutDataLen = 0;
		CK_BYTE* pOutData = Vector2Buffer(outData, ulOutDataLen);

		trace_msg("C_Decrypt");
		rv = Pkcs11Functions->C_Decrypt(hSession, pInData, ulInDataLen, pOutData,
			&ulOutDataLen);

		trace_msg("buffer to vector");
		trace_msg(ulOutDataLen);
		trace_msg(ulInDataLen);
		string s(pOutData, pOutData+2);
		trace_msg(s);
		if (CKR_OK == rv) {
			Buffer2Vector(pOutData, ulOutDataLen, outData, true);
		} else {
			trace_msg("nok");
		}
		if (pOutData)
			delete []pOutData;
		if (pInData)
			delete []pInData;

		return rv;
	}

	unsigned char * SecurityToken::Vector2Buffer(vector<unsigned char> &Buf, CK_ULONG &Len)
	{
		Len = (CK_ULONG)Buf.size();
		trace_msg(Len);
		if (!Len)
			return NULL;
		CK_ULONG i;
		unsigned char *pBuf = new unsigned char[Len];
		for (i = 0; i<Len; i++)
			pBuf[i] = Buf[i];
		return pBuf;
	}

	void SecurityToken::Buffer2Vector(unsigned char* pBuf, CK_ULONG Len, vector<unsigned char> &Buf, bool bAllocIfNull)
	{
		trace_msg("clearing buf");
		Buf.clear();
		trace_msg("allocnull");
		if (!pBuf & bAllocIfNull)
			Buf = vector<unsigned char>(Len);
		else
		{
			trace_msg("reserve");
			Buf.reserve(Len);
			CK_ULONG i;
			trace_msg("pushing");
			for (i = 0; i<Len; i++)
				Buf.push_back(pBuf[i]);
		}
	}

	void SecurityToken::GetDecryptedData(SecurityTokenKey key, vector<byte> tokenDataToDecrypt, vector<byte> &decryptedData)
	{
		GetDecryptedData(key.SlotId, key.Handle, tokenDataToDecrypt, decryptedData);
	}

	void SecurityToken::GetDecryptedData (CK_SLOT_ID slotId, CK_OBJECT_HANDLE tokenObject, vector <byte> edata, vector <byte> &decryptedData)
	{
		LoginUserIfRequired (slotId);

		if (Sessions.find (slotId) == Sessions.end())
			throw ParameterIncorrect (SRC_POS);

		CK_MECHANISM m = { CKM_RSA_PKCS, 0, 0 };
		trace_msg("C_DecryptInit");
		CK_RV status = Pkcs11Functions->C_DecryptInit (Sessions[slotId].Handle, &m, tokenObject);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		trace_msg("PKCS11Decrypt");
		status = SecurityToken::PKCS11Decrypt(
			Sessions[slotId].Handle,
			edata,
			decryptedData
		);

		if (status != CKR_OK)
			throw Pkcs11Exception (status);

	}

	void SecurityToken::GetObjectAttribute (CK_SLOT_ID slotId, CK_OBJECT_HANDLE tokenObject, CK_ATTRIBUTE_TYPE attributeType, vector <byte> &attributeValue)
	{
		attributeValue.clear();

		if (Sessions.find (slotId) == Sessions.end())
			throw ParameterIncorrect (SRC_POS);

		CK_ATTRIBUTE attribute;
		attribute.type = attributeType;
		attribute.pValue = NULL_PTR;

		CK_RV status = Pkcs11Functions->C_GetAttributeValue (Sessions[slotId].Handle, tokenObject, &attribute, 1);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		if (attribute.ulValueLen == 0)
			return;

		attributeValue = vector <byte> (attribute.ulValueLen);
		attribute.pValue = &attributeValue.front();

		status = Pkcs11Functions->C_GetAttributeValue (Sessions[slotId].Handle, tokenObject, &attribute, 1);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);
	}

	list <CK_SLOT_ID> SecurityToken::GetTokenSlots ()
	{
		CheckLibraryStatus();

		list <CK_SLOT_ID> slots;
		CK_ULONG slotCount;

		CK_RV status = Pkcs11Functions->C_GetSlotList (TRUE, NULL_PTR, &slotCount);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		if (slotCount > 0)
		{
			vector <CK_SLOT_ID> slotArray (slotCount);
			status = Pkcs11Functions->C_GetSlotList (TRUE, &slotArray.front(), &slotCount);
			if (status != CKR_OK)
				throw Pkcs11Exception (status);

			for (size_t i = 0; i < slotCount; i++)
			{
				CK_SLOT_INFO slotInfo;
				status = Pkcs11Functions->C_GetSlotInfo (slotArray[i], &slotInfo);

				if (status != CKR_OK || !(slotInfo.flags & CKF_TOKEN_PRESENT))
					continue;

				slots.push_back (slotArray[i]);
			}
		}

		return slots;
	}

	bool SecurityToken::IsKeyfilePathValid (const wstring &securityTokenKeyfilePath)
	{
		return securityTokenKeyfilePath.find (TC_SECURITY_TOKEN_KEYFILE_URL_PREFIX) == 0;
	}

	void SecurityToken::Login (CK_SLOT_ID slotId, const string &pin)
	{
		if (Sessions.find (slotId) == Sessions.end())
			OpenSession (slotId);
		else if (Sessions[slotId].UserLoggedIn)
			return;

		CK_RV status = Pkcs11Functions->C_Login (Sessions[slotId].Handle, CKU_USER, (CK_CHAR_PTR) pin.c_str(), (CK_ULONG) pin.size());

		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		Sessions[slotId].UserLoggedIn = true;
	}
	
	void SecurityToken::LoginUserIfRequired (CK_SLOT_ID slotId)
	{
		trace_msg("checking library status for login");

		CheckLibraryStatus();

		trace_msg("slot id to use is: ");
		trace_msg(slotId);

		CK_RV status;

		if (Sessions.find (slotId) == Sessions.end())
		{
			trace_msg("opening session");
			OpenSession (slotId);
			trace_msg("session is opened");
		}
		else
		{
			CK_SESSION_INFO sessionInfo;
			trace_msg("C_GetSessionInfo");
			status = Pkcs11Functions->C_GetSessionInfo (Sessions[slotId].Handle, &sessionInfo);
			
			if (status == CKR_OK)
			{
				Sessions[slotId].UserLoggedIn = (sessionInfo.state == CKS_RO_USER_FUNCTIONS || sessionInfo.state == CKS_RW_USER_FUNCTIONS);
			}
			else
			{
				try
				{
					CloseSession (slotId);
				}
				catch (...) { }
				OpenSession (slotId);
			}
		}

		SecurityTokenInfo tokenInfo = GetTokenInfo (slotId);

		while (!Sessions[slotId].UserLoggedIn && (tokenInfo.Flags & CKF_LOGIN_REQUIRED))
		{
			try
			{
				if (tokenInfo.Flags & CKF_PROTECTED_AUTHENTICATION_PATH)
				{
					trace_msg("C_Login");
					status = Pkcs11Functions->C_Login (Sessions[slotId].Handle, CKU_USER, NULL_PTR, 0);
					if (status != CKR_OK)
						throw Pkcs11Exception (status);
				}
				else
				{
					string pin = tokenInfo.LabelUtf8;
					if (tokenInfo.Label.empty())
					{
						stringstream s;
						s << "#" << slotId;
						pin = s.str();
					}

					finally_do_arg (string*, &pin, { burn ((void *) finally_arg->c_str(), finally_arg->size()); });

					trace_msg("Trying to do pin callback");
					(*PinCallback) (pin);
					trace_msg("logging in with pin");
					Login (slotId, pin);
				}

				Sessions[slotId].UserLoggedIn = true;
			}
			catch (Pkcs11Exception &e)
			{
				CK_RV error = e.GetErrorCode();

				if (error == CKR_USER_ALREADY_LOGGED_IN)
				{
					break;
				}
				else if (error == CKR_PIN_INCORRECT && !(tokenInfo.Flags & CKF_PROTECTED_AUTHENTICATION_PATH))
				{
					(*WarningCallback) (Pkcs11Exception (CKR_PIN_INCORRECT));
					continue;
				}

				throw;
			}
		}
	}

#ifdef TC_WINDOWS
	void SecurityToken::InitLibrary (const wstring &pkcs11LibraryPath, auto_ptr <GetPinFunctor> pinCallback, auto_ptr <SendExceptionFunctor> warningCallback)
#else
	void SecurityToken::InitLibrary (const string &pkcs11LibraryPath, auto_ptr <GetPinFunctor> pinCallback, auto_ptr <SendExceptionFunctor> warningCallback)
#endif
	{
		if (Initialized)
			CloseLibrary();

#ifdef TC_WINDOWS
		Pkcs11LibraryHandle = LoadLibraryW (pkcs11LibraryPath.c_str());
#else
		Pkcs11LibraryHandle = dlopen (pkcs11LibraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		throw_sys_if (!Pkcs11LibraryHandle);

		typedef CK_RV (*C_GetFunctionList_t) (CK_FUNCTION_LIST_PTR_PTR ppFunctionList);
#ifdef TC_WINDOWS
		C_GetFunctionList_t C_GetFunctionList = (C_GetFunctionList_t) GetProcAddress (Pkcs11LibraryHandle, "C_GetFunctionList");
#else
		C_GetFunctionList_t C_GetFunctionList = (C_GetFunctionList_t) dlsym (Pkcs11LibraryHandle, "C_GetFunctionList");
#endif

		if (!C_GetFunctionList)
			throw SecurityTokenLibraryNotInitialized();

		CK_RV status = C_GetFunctionList (&Pkcs11Functions);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		status = Pkcs11Functions->C_Initialize (NULL_PTR);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		PinCallback = pinCallback;
		WarningCallback = warningCallback;

		Initialized = true;
	}

	void SecurityToken::OpenSession (CK_SLOT_ID slotId)
	{
		if (Sessions.find (slotId) != Sessions.end())
			return;

		CK_SESSION_HANDLE session;

		CK_FLAGS flags = CKF_SERIAL_SESSION;

		if (!(GetTokenInfo (slotId).Flags & CKF_WRITE_PROTECTED))
			 flags |= CKF_RW_SESSION;

		CK_RV status = Pkcs11Functions->C_OpenSession (slotId, flags, NULL_PTR, NULL_PTR, &session);
		if (status != CKR_OK)
			throw Pkcs11Exception (status);

		Sessions[slotId].Handle = session;
	}

	Pkcs11Exception::operator string () const
	{
		if (ErrorCode == CKR_OK)
			return string();

		static const struct
		{
			CK_RV ErrorCode;
			const char *ErrorString;
		} ErrorStrings[] =
		{
#			define TC_TOKEN_ERR(CODE) { CODE, #CODE },

			TC_TOKEN_ERR (CKR_CANCEL)
			TC_TOKEN_ERR (CKR_HOST_MEMORY)
			TC_TOKEN_ERR (CKR_SLOT_ID_INVALID)
			TC_TOKEN_ERR (CKR_GENERAL_ERROR)
			TC_TOKEN_ERR (CKR_FUNCTION_FAILED)
			TC_TOKEN_ERR (CKR_ARGUMENTS_BAD)
			TC_TOKEN_ERR (CKR_NO_EVENT)
			TC_TOKEN_ERR (CKR_NEED_TO_CREATE_THREADS)
			TC_TOKEN_ERR (CKR_CANT_LOCK)
			TC_TOKEN_ERR (CKR_ATTRIBUTE_READ_ONLY)
			TC_TOKEN_ERR (CKR_ATTRIBUTE_SENSITIVE)
			TC_TOKEN_ERR (CKR_ATTRIBUTE_TYPE_INVALID)
			TC_TOKEN_ERR (CKR_ATTRIBUTE_VALUE_INVALID)
			TC_TOKEN_ERR (CKR_DATA_INVALID)
			TC_TOKEN_ERR (CKR_DATA_LEN_RANGE)
			TC_TOKEN_ERR (CKR_DEVICE_ERROR)
			TC_TOKEN_ERR (CKR_DEVICE_MEMORY)
			TC_TOKEN_ERR (CKR_DEVICE_REMOVED)
			TC_TOKEN_ERR (CKR_ENCRYPTED_DATA_INVALID)
			TC_TOKEN_ERR (CKR_ENCRYPTED_DATA_LEN_RANGE)
			TC_TOKEN_ERR (CKR_FUNCTION_CANCELED)
			TC_TOKEN_ERR (CKR_FUNCTION_NOT_PARALLEL)
			TC_TOKEN_ERR (CKR_FUNCTION_NOT_SUPPORTED)
			TC_TOKEN_ERR (CKR_KEY_HANDLE_INVALID)
			TC_TOKEN_ERR (CKR_KEY_SIZE_RANGE)
			TC_TOKEN_ERR (CKR_KEY_TYPE_INCONSISTENT)
			TC_TOKEN_ERR (CKR_KEY_NOT_NEEDED)
			TC_TOKEN_ERR (CKR_KEY_CHANGED)
			TC_TOKEN_ERR (CKR_KEY_NEEDED)
			TC_TOKEN_ERR (CKR_KEY_INDIGESTIBLE)
			TC_TOKEN_ERR (CKR_KEY_FUNCTION_NOT_PERMITTED)
			TC_TOKEN_ERR (CKR_KEY_NOT_WRAPPABLE)
			TC_TOKEN_ERR (CKR_KEY_UNEXTRACTABLE)
			TC_TOKEN_ERR (CKR_MECHANISM_INVALID)
			TC_TOKEN_ERR (CKR_MECHANISM_PARAM_INVALID)
			TC_TOKEN_ERR (CKR_OBJECT_HANDLE_INVALID)
			TC_TOKEN_ERR (CKR_OPERATION_ACTIVE)
			TC_TOKEN_ERR (CKR_OPERATION_NOT_INITIALIZED)
			TC_TOKEN_ERR (CKR_PIN_INCORRECT)
			TC_TOKEN_ERR (CKR_PIN_INVALID)
			TC_TOKEN_ERR (CKR_PIN_LEN_RANGE)
			TC_TOKEN_ERR (CKR_PIN_EXPIRED)
			TC_TOKEN_ERR (CKR_PIN_LOCKED)
			TC_TOKEN_ERR (CKR_SESSION_CLOSED)
			TC_TOKEN_ERR (CKR_SESSION_COUNT)
			TC_TOKEN_ERR (CKR_SESSION_HANDLE_INVALID)
			TC_TOKEN_ERR (CKR_SESSION_PARALLEL_NOT_SUPPORTED)
			TC_TOKEN_ERR (CKR_SESSION_READ_ONLY)
			TC_TOKEN_ERR (CKR_SESSION_EXISTS)
			TC_TOKEN_ERR (CKR_SESSION_READ_ONLY_EXISTS)
			TC_TOKEN_ERR (CKR_SESSION_READ_WRITE_SO_EXISTS)
			TC_TOKEN_ERR (CKR_SIGNATURE_INVALID)
			TC_TOKEN_ERR (CKR_SIGNATURE_LEN_RANGE)
			TC_TOKEN_ERR (CKR_TEMPLATE_INCOMPLETE)
			TC_TOKEN_ERR (CKR_TEMPLATE_INCONSISTENT)
			TC_TOKEN_ERR (CKR_TOKEN_NOT_PRESENT)
			TC_TOKEN_ERR (CKR_TOKEN_NOT_RECOGNIZED)
			TC_TOKEN_ERR (CKR_TOKEN_WRITE_PROTECTED)
			TC_TOKEN_ERR (CKR_UNWRAPPING_KEY_HANDLE_INVALID)
			TC_TOKEN_ERR (CKR_UNWRAPPING_KEY_SIZE_RANGE)
			TC_TOKEN_ERR (CKR_UNWRAPPING_KEY_TYPE_INCONSISTENT)
			TC_TOKEN_ERR (CKR_USER_ALREADY_LOGGED_IN)
			TC_TOKEN_ERR (CKR_USER_NOT_LOGGED_IN)
			TC_TOKEN_ERR (CKR_USER_PIN_NOT_INITIALIZED)
			TC_TOKEN_ERR (CKR_USER_TYPE_INVALID)
			TC_TOKEN_ERR (CKR_USER_ANOTHER_ALREADY_LOGGED_IN)
			TC_TOKEN_ERR (CKR_USER_TOO_MANY_TYPES)
			TC_TOKEN_ERR (CKR_WRAPPED_KEY_INVALID)
			TC_TOKEN_ERR (CKR_WRAPPED_KEY_LEN_RANGE)
			TC_TOKEN_ERR (CKR_WRAPPING_KEY_HANDLE_INVALID)
			TC_TOKEN_ERR (CKR_WRAPPING_KEY_SIZE_RANGE)
			TC_TOKEN_ERR (CKR_WRAPPING_KEY_TYPE_INCONSISTENT)
			TC_TOKEN_ERR (CKR_RANDOM_SEED_NOT_SUPPORTED)
			TC_TOKEN_ERR (CKR_RANDOM_NO_RNG)
			TC_TOKEN_ERR (CKR_DOMAIN_PARAMS_INVALID)
			TC_TOKEN_ERR (CKR_BUFFER_TOO_SMALL)
			TC_TOKEN_ERR (CKR_SAVED_STATE_INVALID)
			TC_TOKEN_ERR (CKR_INFORMATION_SENSITIVE)
			TC_TOKEN_ERR (CKR_STATE_UNSAVEABLE)
			TC_TOKEN_ERR (CKR_CRYPTOKI_NOT_INITIALIZED)
			TC_TOKEN_ERR (CKR_CRYPTOKI_ALREADY_INITIALIZED)
			TC_TOKEN_ERR (CKR_MUTEX_BAD)
			TC_TOKEN_ERR (CKR_MUTEX_NOT_LOCKED)
			TC_TOKEN_ERR (CKR_NEW_PIN_MODE)
			TC_TOKEN_ERR (CKR_NEXT_OTP)
			TC_TOKEN_ERR (CKR_FUNCTION_REJECTED)

#undef		TC_TOKEN_ERR
		};


		for (size_t i = 0; i < array_capacity (ErrorStrings); ++i)
		{
			if (ErrorStrings[i].ErrorCode == ErrorCode)
				return ErrorStrings[i].ErrorString;
		}

		stringstream s;
		s << "0x" << hex << ErrorCode;
		return s.str();

	}

#ifdef TC_HEADER_Common_Exception
	void Pkcs11Exception::Show (HWND parent) const
	{
		string errorString = string (*this);

		if (!errorString.empty())
		{
			wstringstream subjectErrorCode;
			if (SubjectErrorCodeValid)
				subjectErrorCode << L": " << SubjectErrorCode;

			if (!GetDictionaryValue (errorString.c_str()))
			{
				if (errorString.find ("CKR_") == 0)
				{
					errorString = errorString.substr (4);
					for (size_t i = 0; i < errorString.size(); ++i)
					{
						if (errorString[i] == '_')
							errorString[i] = ' ';
					}
				}
				wchar_t err[8192];
				StringCbPrintfW (err, sizeof(err),L"%s:\n\n%hs%s", GetString ("SECURITY_TOKEN_ERROR"), errorString.c_str(), subjectErrorCode.str().c_str());
				ErrorDirect (err, parent);
			}
			else
			{
				wstring err = GetString (errorString.c_str());

				if (SubjectErrorCodeValid)
					err += L"\n\nError code" + subjectErrorCode.str();

				ErrorDirect (err.c_str(), parent);
			}
		}
	}
#endif // TC_HEADER_Common_Exception

	auto_ptr <GetPinFunctor> SecurityToken::PinCallback;
	auto_ptr <SendExceptionFunctor> SecurityToken::WarningCallback;

	bool SecurityToken::Initialized;
	CK_FUNCTION_LIST_PTR SecurityToken::Pkcs11Functions;
	map <CK_SLOT_ID, Pkcs11Session> SecurityToken::Sessions;

#ifdef TC_WINDOWS
	HMODULE SecurityToken::Pkcs11LibraryHandle;
#else
	void *SecurityToken::Pkcs11LibraryHandle;
#endif

#ifdef TC_HEADER_Platform_Exception

	void Pkcs11Exception::Deserialize (shared_ptr <Stream> stream)
	{
		Exception::Deserialize (stream);
		Serializer sr (stream);
		uint64 code;
		sr.Deserialize ("ErrorCode", code);
		sr.Deserialize ("SubjectErrorCodeValid", SubjectErrorCodeValid);
		sr.Deserialize ("SubjectErrorCode", SubjectErrorCode);
		ErrorCode = (CK_RV) code;
	}

	void Pkcs11Exception::Serialize (shared_ptr <Stream> stream) const
	{
		Exception::Serialize (stream);
		Serializer sr (stream);
		sr.Serialize ("ErrorCode", (uint64) ErrorCode);
		sr.Serialize ("SubjectErrorCodeValid", SubjectErrorCodeValid);
		sr.Serialize ("SubjectErrorCode", SubjectErrorCode);
	}

#	define TC_EXCEPTION(TYPE) TC_SERIALIZER_FACTORY_ADD(TYPE)
#	undef TC_EXCEPTION_NODECL
#	define TC_EXCEPTION_NODECL(TYPE) TC_SERIALIZER_FACTORY_ADD(TYPE)

	TC_SERIALIZER_FACTORY_ADD_EXCEPTION_SET (SecurityTokenException);

#endif
}
