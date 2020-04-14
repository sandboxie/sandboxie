//---------------------------------------------------------------------------
// Low Level Binary
//---------------------------------------------------------------------------

#ifdef _WIN64

LOWLEVEL  RCDATA     "../low/obj/amd64/LowLevel.dll"

#else // ! _WIN64

LOWLEVEL  RCDATA     "../low/obj/i386/LowLevel.dll"

#endif // _WIN64
