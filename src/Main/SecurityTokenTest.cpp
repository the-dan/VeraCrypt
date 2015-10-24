
#include <Common/SecurityToken.h>

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

	unsigned long slotId = 1;
	wstring Id = L"KEY MAN key";
	vector <SecurityTokenKeyfile> keys = SecurityToken::GetAvailableKeys (&slotId, Id);

	trace_msg(keys.size());

	SecurityTokenKeyfile key = keys.front();

	vector <byte> keyfileData;
	try {
		SecurityToken::GetKeyfileData(key, keyfileData);
	} catch (Exception &ex) {
		if (dynamic_cast <const Pkcs11Exception *> (&ex))
		{
			string errorString = string (dynamic_cast <const Pkcs11Exception &> (ex));
			

			trace_msg(errorString);
		}
	}

}
