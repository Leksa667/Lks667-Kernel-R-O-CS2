// =============================================================================
// Lks667 Kernel RO CS2
// By Leksa667 - 12/08/2026
// Application Windows experimentale : interface UM, overlay et composant kernel.
// =============================================================================

#pragma once

namespace crypt
{
    constexpr unsigned long long linear_congruent_generator(unsigned rounds)
    {
        return 10133792423ull + (166167355ull * ((rounds > 0) ? linear_congruent_generator(rounds - 1) : 6637ull)) % 0xBFC66BFE;
    }

    template <unsigned size, typename Char, unsigned int Seed = 0>
    class Xor_string {
    public:
        static constexpr unsigned long long XORKEY = (linear_congruent_generator(Seed % 10 + 2) ^ (Seed * 16777619ull)) % 0xFF;

        const unsigned _nb_chars = (size - 1);
        Char _string[size];
        mutable bool _decrypted = false;

        static constexpr Char encrypt_character(const Char character, int index)
        {
            return static_cast<Char>(character ^ (static_cast<Char>(XORKEY == 0 ? 0x4B : XORKEY) + index));
        }

        inline constexpr Xor_string(const Char* string)
            : _string{}, _decrypted{false}
        {
            for (unsigned i = 0u; i < size; ++i)
                _string[i] = encrypt_character(string[i], i);
        }

        const Char* decrypt() const
        {
            if (_decrypted)
                return _string;

            Char* string = const_cast<Char*>(_string);
            for (unsigned t = 0; t < _nb_chars; t++)
            {
                string[t] = static_cast<Char>(string[t] ^ (static_cast<Char>(XORKEY == 0 ? 0x4B : XORKEY) + t));
            }
            string[_nb_chars] = '\0';
            _decrypted = true;
            return string;
        }
    };
}
#define XorS(name, my_string)    crypt::Xor_string<(sizeof(my_string)/sizeof(char)), char, __COUNTER__> name(my_string)
#define XorString(my_string) []{ constexpr crypt::Xor_string<(sizeof(my_string)/sizeof(char)), char, __COUNTER__> expr(my_string); return expr; }().decrypt()
#define EEW( string ) XorString( string )

#define XorWS(name, my_string)   crypt::Xor_string<(sizeof(my_string)/sizeof(wchar_t)), wchar_t, __COUNTER__> name(my_string)
#define XorWideString(my_string) []{ constexpr crypt::Xor_string<(sizeof(my_string)/sizeof(wchar_t)), wchar_t, __COUNTER__> expr(my_string); return expr; }().decrypt()
#define EW( string ) XorWideString( string )