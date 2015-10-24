
#include <Common/SecurityToken.h>
#include <Platform/File.h>
#include <Platform/FilesystemPath.h>

using namespace VeraCrypt;


struct PinRequestHandler : public GetPinFunctor
{

	virtual void operator() (string &passwordStr)
	{
		passwordStr = "817634";	
	}
};

struct WarningHandler : public SendExceptionFunctor
{
	//WarningHandler (const TextUserInterface *userInterface) : UI (userInterface) { }

	virtual void operator() (const Exception &ex)
	{	
		/*
		const Pkcs11Exception *ee = reinterpret_cast<const Pkcs11Exception*>(&e);
		string s = *ee;
		trace_msg(s);
		*/

		if (dynamic_cast <const Pkcs11Exception *> (&ex))
		{
			string errorString = string (dynamic_cast <const Pkcs11Exception &> (ex));
			

			trace_msg(errorString);
		}

	}

	//const TextUserInterface *UI;
};

int main(int argc, char** argv) {

	trace_msgw(L"starting");


	string libPath = string("/usr/lib/opensc-pkcs11.so");
	PinRequestHandler pinCallback;
	WarningHandler warningCallback;
	SecurityToken::InitLibrary (libPath,
		auto_ptr <GetPinFunctor> (new PinRequestHandler ()),
		auto_ptr <SendExceptionFunctor> (new WarningHandler ())
	);

	/*
	unsigned long slotId = 1;
	wstring Id = L"KEY MAN key";
	vector <SecurityTokenKey> keys = SecurityToken::GetAvailableKeys (&slotId, Id);

	trace_msg(keys.size());

	SecurityTokenKey key = keys.front();
	*/

	SecurityTokenKey key;
	SecurityToken::GetSecurityTokenKey(L"1:KEY MAN key", key);

	vector <byte> encryptedData;
	vector <byte> decryptedData(key.maxDecryptBufferSize, 0);
	encryptedData.reserve(key.maxEncryptBufferSize);

	File file;
	uint64 totalLength = 0;
	uint64 readLength;


	FilesystemPath Path("/Users/dan/projects/bluekey/pkcs11-attempt/openssl-encrypted-text.blob");
	SecureBuffer keyfileBuf (File::GetOptimalReadSize());
	trace_msg("opening file");
	file.Open (Path, File::OpenRead, File::ShareRead);
	trace_msg("opened");

	


	while ((readLength = file.Read (keyfileBuf)) > 0)
	{
		byte *data = keyfileBuf.Ptr();
		encryptedData.insert(encryptedData.end(), data, data + readLength);

		string log_data(data, data+readLength);
		trace_msg(log_data);
	}


	trace_msg("decrypting");
	trace_msg(encryptedData.size());
	// trace_msg(readLength);


	try {
		SecurityToken::GetDecryptedData(key, encryptedData, decryptedData);
	} catch (Exception &ex) {
		if (dynamic_cast <const Pkcs11Exception *> (&ex))
		{
			string errorString = string (dynamic_cast <const Pkcs11Exception &> (ex));
			

			trace_msg(errorString);
		}
	}
	trace_msg("decrypted result");
	trace_msg(decryptedData.size());

	string s(decryptedData.data(), decryptedData.data() + decryptedData.size());
	trace_msg(s);


}
