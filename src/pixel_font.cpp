#include "pixel_font.hpp"

#include <cctype>

namespace fpkgi {
namespace {

const unsigned char* glyph(char input) {
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(input)));
    static const unsigned char blank[7]={0,0,0,0,0,0,0};
    static const unsigned char unknown[7]={14,17,1,2,4,0,4};
    static const unsigned char A[7]={14,17,17,31,17,17,17};
    static const unsigned char B[7]={30,17,17,30,17,17,30};
    static const unsigned char C[7]={14,17,16,16,16,17,14};
    static const unsigned char D[7]={30,17,17,17,17,17,30};
    static const unsigned char E[7]={31,16,16,30,16,16,31};
    static const unsigned char F[7]={31,16,16,30,16,16,16};
    static const unsigned char G[7]={14,17,16,23,17,17,15};
    static const unsigned char H[7]={17,17,17,31,17,17,17};
    static const unsigned char I[7]={14,4,4,4,4,4,14};
    static const unsigned char J[7]={7,2,2,2,18,18,12};
    static const unsigned char K[7]={17,18,20,24,20,18,17};
    static const unsigned char L[7]={16,16,16,16,16,16,31};
    static const unsigned char M[7]={17,27,21,21,17,17,17};
    static const unsigned char N[7]={17,25,21,19,17,17,17};
    static const unsigned char O[7]={14,17,17,17,17,17,14};
    static const unsigned char P[7]={30,17,17,30,16,16,16};
    static const unsigned char Q[7]={14,17,17,17,21,18,13};
    static const unsigned char R[7]={30,17,17,30,20,18,17};
    static const unsigned char S[7]={15,16,16,14,1,1,30};
    static const unsigned char T[7]={31,4,4,4,4,4,4};
    static const unsigned char U[7]={17,17,17,17,17,17,14};
    static const unsigned char V[7]={17,17,17,17,17,10,4};
    static const unsigned char W[7]={17,17,17,21,21,21,10};
    static const unsigned char X[7]={17,17,10,4,10,17,17};
    static const unsigned char Y[7]={17,17,10,4,4,4,4};
    static const unsigned char Z[7]={31,1,2,4,8,16,31};
    static const unsigned char N0[7]={14,17,19,21,25,17,14};
    static const unsigned char N1[7]={4,12,4,4,4,4,14};
    static const unsigned char N2[7]={14,17,1,2,4,8,31};
    static const unsigned char N3[7]={30,1,1,14,1,1,30};
    static const unsigned char N4[7]={2,6,10,18,31,2,2};
    static const unsigned char N5[7]={31,16,16,30,1,1,30};
    static const unsigned char N6[7]={14,16,16,30,17,17,14};
    static const unsigned char N7[7]={31,1,2,4,8,8,8};
    static const unsigned char N8[7]={14,17,17,14,17,17,14};
    static const unsigned char N9[7]={14,17,17,15,1,1,14};
    static const unsigned char DASH[7]={0,0,0,31,0,0,0};
    static const unsigned char DOT[7]={0,0,0,0,0,12,12};
    static const unsigned char COLON[7]={0,12,12,0,12,12,0};
    static const unsigned char SLASH[7]={1,2,2,4,8,8,16};
    static const unsigned char UNDER[7]={0,0,0,0,0,0,31};
    static const unsigned char LP[7]={2,4,8,8,8,4,2};
    static const unsigned char RP[7]={8,4,2,2,2,4,8};
    static const unsigned char PLUS[7]={0,4,4,31,4,4,0};
    static const unsigned char PERCENT[7]={17,2,4,8,17,0,0};
    static const unsigned char EXCL[7]={4,4,4,4,4,0,4};
    switch (c) {
        case ' ': return blank; case 'A': return A; case 'B': return B; case 'C': return C;
        case 'D': return D; case 'E': return E; case 'F': return F; case 'G': return G;
        case 'H': return H; case 'I': return I; case 'J': return J; case 'K': return K;
        case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O;
        case 'P': return P; case 'Q': return Q; case 'R': return R; case 'S': return S;
        case 'T': return T; case 'U': return U; case 'V': return V; case 'W': return W;
        case 'X': return X; case 'Y': return Y; case 'Z': return Z;
        case '0': return N0; case '1': return N1; case '2': return N2; case '3': return N3;
        case '4': return N4; case '5': return N5; case '6': return N6; case '7': return N7;
        case '8': return N8; case '9': return N9; case '-': return DASH; case '.': return DOT;
        case ':': return COLON; case '/': return SLASH; case '_': return UNDER; case '(': return LP;
        case ')': return RP; case '+': return PLUS; case '%': return PERCENT; case '!': return EXCL;
        default: return unknown;
    }
}

} // namespace

void draw_text(SDL_Renderer* renderer, int x, int y, int scale, const std::string& text, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int cursor = x;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char* rows = glyph(text[i]);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (rows[row] & (1 << (4 - col))) {
                    SDL_Rect pixel = {cursor + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }
        cursor += 6 * scale;
    }
}

int text_width(int scale, const std::string& text) {
    return static_cast<int>(text.size()) * 6 * scale;
}

} // namespace fpkgi
