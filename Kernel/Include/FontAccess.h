#pragma once

#include <Types.h>

#include <Lib/Font/English.h>
#include <Lib/Font/Numbers.h>
#include <Lib/Font/Symbols.h>

// ========== Quick access tables ==========
static const UINT8 (*FontUppercase[26])[16] = {
    &A, &B, &C, &D, &E, &F, &G, &H, &I, &J,
    &K, &L, &M, &N, &O, &P, &Q, &R, &S, &T,
    &U, &V, &W, &X, &Y, &Z
};

static const UINT8 (*FontLowercase[26])[16] = {
    &a, &b, &c, &d, &e, &f, &g, &h, &i, &j,
    &k, &l, &m, &n, &o, &p, &q, &r, &s, &t,
    &u, &v, &w, &x, &y, &z
};

static const UINT8 (*FontNumbers[10])[16] = {
    &Digit0, &Digit1, &Digit2, &Digit3, &Digit4,
    &Digit5, &Digit6, &Digit7, &Digit8, &Digit9
};

// ========== Function for obtaining a symbol by code (0x00-0xFF) ==========
static const UINT8 (*FontGetGlyphByCode(UINT8 Code))[16] {
    // A-Z (0x41-0x5A)
    if (Code >= 0x41 && Code <= 0x5A) {
        return FontUppercase[Code - 0x41];
    }
    // a-z (0x61-0x7A)
    if (Code >= 0x61 && Code <= 0x7A) {
        return FontLowercase[Code - 0x61];
    }
    // 0-9 (0x30-0x39)
    if (Code >= 0x30 && Code <= 0x39) {
        return FontNumbers[Code - 0x30];
    }
    
    // All other characters by code
    switch (Code) {
        case 0x00: return &Sym_Null;
        case 0x01: return &Sym_Soh;
        case 0x02: return &Sym_Stx;
        case 0x03: return &Sym_Etx;
        case 0x04: return &Sym_Eot;
        case 0x05: return &Sym_Enq;
        case 0x06: return &Sym_Ack;
        case 0x07: return &Sym_Bel;
        case 0x08: return &Sym_Bs;
        case 0x09: return &Sym_Ht;
        case 0x0A: return &Sym_Lf;
        case 0x0B: return &Sym_Vt;
        case 0x0C: return &Sym_Ff;
        case 0x0D: return &Sym_Cr;
        case 0x0E: return &Sym_So;
        case 0x0F: return &Sym_Si;
        case 0x10: return &Sym_Dle;
        case 0x11: return &Sym_Dc1;
        case 0x12: return &Sym_Dc2;
        case 0x13: return &Sym_Dc3;
        case 0x14: return &Sym_Dc4;
        case 0x15: return &Sym_Nak;
        case 0x16: return &Sym_Syn;
        case 0x17: return &Sym_Etb;
        case 0x18: return &Sym_Can;
        case 0x19: return &Sym_Em;
        case 0x1A: return &Sym_Sub;
        case 0x1B: return &Sym_Esc;
        case 0x1C: return &Sym_Fs;
        case 0x1D: return &Sym_Gs;
        case 0x1E: return &Sym_Rs;
        case 0x1F: return &Sym_Us;
        case 0x20: return &Space;
        case 0x21: return &Exclamation;
        case 0x22: return &DoubleQuote;
        case 0x23: return &Hash;
        case 0x24: return &Dollar;
        case 0x25: return &Percent;
        case 0x26: return &Ampersand;
        case 0x27: return &Apostrophe;
        case 0x28: return &LeftParen;
        case 0x29: return &RightParen;
        case 0x2A: return &Asterisk;
        case 0x2B: return &Plus;
        case 0x2C: return &Comma;
        case 0x2D: return &Minus;
        case 0x2E: return &Period;
        case 0x2F: return &Slash;
        case 0x3A: return &Colon;
        case 0x3B: return &Semicolon;
        case 0x3C: return &Less;
        case 0x3D: return &Equal;
        case 0x3E: return &Greater;
        case 0x3F: return &Question;
        case 0x40: return &At;
        case 0x5B: return &LeftBracket;
        case 0x5C: return &Backslash;
        case 0x5D: return &RightBracket;
        case 0x5E: return &Caret;
        case 0x5F: return &Underscore;
        case 0x60: return &Grave;
        case 0x7B: return &LeftBrace;
        case 0x7C: return &VerticalBar;
        case 0x7D: return &RightBrace;
        case 0x7E: return &Tilde;
        case 0x7F: return &Del;
        
        // Extended characters (0x80-0xFF)
        case 0x80: return &C_Cedilla;
        case 0x81: return &U_Diaeresis;
        case 0x82: return &E_Acute;
        case 0x83: return &A_Circumflex;
        case 0x84: return &A_Diaeresis;
        case 0x85: return &A_Grave;
        case 0x86: return &A_Ring;
        case 0x87: return &C_CedillaSmall;
        case 0x88: return &E_Circumflex;
        case 0x89: return &E_Diaeresis;
        case 0x8A: return &E_Grave;
        case 0x8B: return &I_Diaeresis;
        case 0x8C: return &I_Circumflex;
        case 0x8D: return &I_Grave;
        case 0x8E: return &A_DiaeresisCap;
        case 0x8F: return &A_RingCap;
        case 0x90: return &E_AcuteCap;
        case 0x91: return &Ae;
        case 0x92: return &AeCap;
        case 0x93: return &O_Circumflex;
        case 0x94: return &O_Diaeresis;
        case 0x95: return &O_Grave;
        case 0x96: return &U_Circumflex;
        case 0x97: return &U_Grave;
        case 0x98: return &Y_Diaeresis;
        case 0x99: return &O_DiaeresisCap;
        case 0x9A: return &U_DiaeresisCap;
        case 0x9B: return &Cent;
        case 0x9C: return &Pound;
        case 0x9D: return &Yen;
        case 0x9E: return &Peseta;
        case 0x9F: return &Florin;
        case 0xA0: return &A_Acute;
        case 0xA1: return &I_Acute;
        case 0xA2: return &O_Acute;
        case 0xA3: return &U_Acute;
        case 0xA4: return &N_Tilde;
        case 0xA5: return &N_TildeCap;
        case 0xA6: return &FeminineOrdinal;
        case 0xA7: return &MasculineOrdinal;
        case 0xA8: return &InvertedQuestion;
        case 0xA9: return &ReverseNot;
        case 0xAA: return &Not;
        case 0xAB: return &OneHalf;
        case 0xAC: return &OneQuarter;
        case 0xAD: return &InvertedExclamation;
        case 0xAE: return &LeftAngleQuote;
        case 0xAF: return &RightAngleQuote;
        case 0xB0: return &LightShade;
        case 0xB1: return &MediumShade;
        case 0xB2: return &DarkShade;
        case 0xB3: return &VerticalBarLight;
        case 0xB4: return &TeeLeft;
        case 0xB5: return &TeeLeftDouble;
        case 0xB6: return &TeeUpDouble;
        case 0xB7: return &TeeUp;
        case 0xB8: return &TeeRight;
        case 0xB9: return &TeeUpDoubleCross;
        case 0xBA: return &DoubleVertical;
        case 0xBB: return &TeeUpRight;
        case 0xBC: return &TeeDownRight;
        case 0xBD: return &TeeDownRightDouble;
        case 0xBE: return &TeeRightLight;
        case 0xBF: return &TeeDownLight;
        case 0xC0: return &TeeRightLightUp;
        case 0xC1: return &TeeDownLightUp;
        case 0xC2: return &TeeUpLight;
        case 0xC3: return &TeeLeftLight;
        case 0xC4: return &HorizontalLine;
        case 0xC5: return &CrossLight;
        case 0xC6: return &TeeLeftLightDouble;
        case 0xC7: return &TeeUpLightDouble;
        case 0xC8: return &TeeRightDoubleLower;
        case 0xC9: return &TeeDownDoubleUpper;
        case 0xCA: return &CrossDoubleVertical;
        case 0xCB: return &CrossDoubleHorizontal;
        case 0xCC: return &TeeUpLightDoubleAlt;
        case 0xCD: return &DoubleHorizontal;
        case 0xCE: return &DoubleCross;
        case 0xCF: return &CrossLightTop;
        case 0xD0: return &TeeLeftLightTop;
        case 0xD1: return &TeeLeftDoubleHorizontal;
        case 0xD2: return &TeeUpDouble;
        case 0xD3: return &TeeRightLightLower;
        case 0xD4: return &TeeRightLightUpper;
        case 0xD5: return &TeeLeftLightLower;
        case 0xD6: return &TeeDownLight;
        case 0xD7: return &CrossDouble;
        case 0xD8: return &CrossLightDouble;
        case 0xD9: return &TeeUpLightRight;
        case 0xDA: return &TeeDownLightLeft;
        case 0xDB: return &FullBlock;
        case 0xDC: return &LowerHalfBlock;
        case 0xDD: return &LeftHalfBlock;
        case 0xDE: return &RightHalfBlock;
        case 0xDF: return &UpperHalfBlock;
        case 0xE0: return &Alpha;
        case 0xE1: return &Beta;
        case 0xE2: return &Gamma;
        case 0xE3: return &Pi;
        case 0xE4: return &Sigma;
        case 0xE5: return &SigmaLower;
        case 0xE6: return &Mu;
        case 0xE7: return &Tau;
        case 0xE8: return &Phi;
        case 0xE9: return &Theta;
        case 0xEA: return &Omega;
        case 0xEB: return &Delta;
        case 0xEC: return &Infinity;
        case 0xED: return &PhiLower;
        case 0xEE: return &Epsilon;
        case 0xEF: return &Intersection;
        case 0xF0: return &Equivalence;
        case 0xF1: return &PlusMinus;
        case 0xF2: return &GreaterEqual;
        case 0xF3: return &LessEqual;
        case 0xF4: return &TopHalfIntegral;
        case 0xF5: return &BottomHalfIntegral;
        case 0xF6: return &Division;
        case 0xF7: return &Approximately;
        case 0xF8: return &Degree;
        case 0xF9: return &MiddleDot;
        case 0xFA: return &Bullet;
        case 0xFB: return &SquareRoot;
        case 0xFC: return &SuperscriptN;
        case 0xFD: return &Superscript2;
        case 0xFE: return &BlackSquare;
        case 0xFF: return &NoBreakSpace;
        
        default: return &Space;
    }
}

// ========== Convenient function for getting by char ==========
static const UINT8 (*FontGetGlyph(CHAR C))[16] {
    return FontGetGlyphByCode((UINT8)C);
}