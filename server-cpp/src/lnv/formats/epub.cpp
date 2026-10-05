// EPUB 适配器实现（照抄 server/app/formats/epub.py 的 zip + 手写 OPF/spine 路径）
#include "lnv/formats/epub.h"

#include <list>  // deque 不支持不完整类型，list 支持（HNode 递归）
#include <miniz.h>
#include <pugixml.hpp>

#include "lnv/formats/comics.h"     // ComicAdapter（图片型 EPUB 分流）
#include "lnv/formats/container.h"  // decode_zip_name（条目名 cp437 还原）
#include "lnv/render/raster.h"      // text_cover：坏书封面退化（对齐 Python）
#include "lnv/util/imaging.h"
#include "lnv/util/paths.h"

namespace lnv {
namespace {

// ============================================================ posix 路径工具
std::string posix_dirname(const std::string& p) {
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return "";
    return p.substr(0, pos);
}

std::string posix_basename(const std::string& p) {
    size_t pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

// 词法规范化（处理 "." 与 ".."），对应 posixpath.normpath（不含 Windows 盘符语义）
std::string posix_normpath(const std::string& p) {
    if (p.empty()) return ".";
    bool abs = !p.empty() && p[0] == '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!abs) parts.push_back("..");
            continue;
        }
        parts.push_back(seg);
    }
    std::string out = abs ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += "/";
        out += parts[k];
    }
    if (out.empty()) out = abs ? "/" : ".";
    return out;
}

std::string posix_join_norm(const std::string& base_dir, const std::string& href) {
    if (href.empty()) return posix_normpath(base_dir);
    if (base_dir.empty()) return posix_normpath(href);
    return posix_normpath(base_dir + "/" + href);
}

std::string to_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// ============================================================ 实体解码
// Python html.unescape 等价：数字引用（含 &#x..;、无分号与非法码点规则）+ HTML5
// 命名实体全表（含遗留的无分号形式与最长前缀匹配）。epub.py 的文本/属性都经
// BeautifulSoup 解码；只解 amp/lt/gt/quot/apos 会让 &nbsp; &mdash; 以原文进入
// 正文并影响分页（审查 M5）。
struct HtmlEntity {
    const char* name;   // 含 ';' 形式与 HTML5 遗留的无分号别名
    const char* value;  // UTF-8（\xNN 转义，避免源码编码差异）
};

// Python html.entities.html5 全表，按字节序（= Python sorted() 码点序）排列
constexpr HtmlEntity kHtml5Entities[] = {
{"AElig", "\xC3\x86"},{"AElig;", "\xC3\x86"},{"AMP", "\x26"},{"AMP;", "\x26"},
{"Aacute", "\xC3\x81"},{"Aacute;", "\xC3\x81"},{"Abreve;", "\xC4\x82"},{"Acirc", "\xC3\x82"},
{"Acirc;", "\xC3\x82"},{"Acy;", "\xD0\x90"},{"Afr;", "\xF0\x9D\x94\x84"},{"Agrave", "\xC3\x80"},
{"Agrave;", "\xC3\x80"},{"Alpha;", "\xCE\x91"},{"Amacr;", "\xC4\x80"},{"And;", "\xE2\xA9\x93"},
{"Aogon;", "\xC4\x84"},{"Aopf;", "\xF0\x9D\x94\xB8"},{"ApplyFunction;", "\xE2\x81\xA1"},{"Aring", "\xC3\x85"},
{"Aring;", "\xC3\x85"},{"Ascr;", "\xF0\x9D\x92\x9C"},{"Assign;", "\xE2\x89\x94"},{"Atilde", "\xC3\x83"},
{"Atilde;", "\xC3\x83"},{"Auml", "\xC3\x84"},{"Auml;", "\xC3\x84"},{"Backslash;", "\xE2\x88\x96"},
{"Barv;", "\xE2\xAB\xA7"},{"Barwed;", "\xE2\x8C\x86"},{"Bcy;", "\xD0\x91"},{"Because;", "\xE2\x88\xB5"},
{"Bernoullis;", "\xE2\x84\xAC"},{"Beta;", "\xCE\x92"},{"Bfr;", "\xF0\x9D\x94\x85"},{"Bopf;", "\xF0\x9D\x94\xB9"},
{"Breve;", "\xCB\x98"},{"Bscr;", "\xE2\x84\xAC"},{"Bumpeq;", "\xE2\x89\x8E"},{"CHcy;", "\xD0\xA7"},
{"COPY", "\xC2\xA9"},{"COPY;", "\xC2\xA9"},{"Cacute;", "\xC4\x86"},{"Cap;", "\xE2\x8B\x92"},
{"CapitalDifferentialD;", "\xE2\x85\x85"},{"Cayleys;", "\xE2\x84\xAD"},{"Ccaron;", "\xC4\x8C"},{"Ccedil", "\xC3\x87"},
{"Ccedil;", "\xC3\x87"},{"Ccirc;", "\xC4\x88"},{"Cconint;", "\xE2\x88\xB0"},{"Cdot;", "\xC4\x8A"},
{"Cedilla;", "\xC2\xB8"},{"CenterDot;", "\xC2\xB7"},{"Cfr;", "\xE2\x84\xAD"},{"Chi;", "\xCE\xA7"},
{"CircleDot;", "\xE2\x8A\x99"},{"CircleMinus;", "\xE2\x8A\x96"},{"CirclePlus;", "\xE2\x8A\x95"},{"CircleTimes;", "\xE2\x8A\x97"},
{"ClockwiseContourIntegral;", "\xE2\x88\xB2"},{"CloseCurlyDoubleQuote;", "\xE2\x80\x9D"},{"CloseCurlyQuote;", "\xE2\x80\x99"},{"Colon;", "\xE2\x88\xB7"},
{"Colone;", "\xE2\xA9\xB4"},{"Congruent;", "\xE2\x89\xA1"},{"Conint;", "\xE2\x88\xAF"},{"ContourIntegral;", "\xE2\x88\xAE"},
{"Copf;", "\xE2\x84\x82"},{"Coproduct;", "\xE2\x88\x90"},{"CounterClockwiseContourIntegral;", "\xE2\x88\xB3"},{"Cross;", "\xE2\xA8\xAF"},
{"Cscr;", "\xF0\x9D\x92\x9E"},{"Cup;", "\xE2\x8B\x93"},{"CupCap;", "\xE2\x89\x8D"},{"DD;", "\xE2\x85\x85"},
{"DDotrahd;", "\xE2\xA4\x91"},{"DJcy;", "\xD0\x82"},{"DScy;", "\xD0\x85"},{"DZcy;", "\xD0\x8F"},
{"Dagger;", "\xE2\x80\xA1"},{"Darr;", "\xE2\x86\xA1"},{"Dashv;", "\xE2\xAB\xA4"},{"Dcaron;", "\xC4\x8E"},
{"Dcy;", "\xD0\x94"},{"Del;", "\xE2\x88\x87"},{"Delta;", "\xCE\x94"},{"Dfr;", "\xF0\x9D\x94\x87"},
{"DiacriticalAcute;", "\xC2\xB4"},{"DiacriticalDot;", "\xCB\x99"},{"DiacriticalDoubleAcute;", "\xCB\x9D"},{"DiacriticalGrave;", "\x60"},
{"DiacriticalTilde;", "\xCB\x9C"},{"Diamond;", "\xE2\x8B\x84"},{"DifferentialD;", "\xE2\x85\x86"},{"Dopf;", "\xF0\x9D\x94\xBB"},
{"Dot;", "\xC2\xA8"},{"DotDot;", "\xE2\x83\x9C"},{"DotEqual;", "\xE2\x89\x90"},{"DoubleContourIntegral;", "\xE2\x88\xAF"},
{"DoubleDot;", "\xC2\xA8"},{"DoubleDownArrow;", "\xE2\x87\x93"},{"DoubleLeftArrow;", "\xE2\x87\x90"},{"DoubleLeftRightArrow;", "\xE2\x87\x94"},
{"DoubleLeftTee;", "\xE2\xAB\xA4"},{"DoubleLongLeftArrow;", "\xE2\x9F\xB8"},{"DoubleLongLeftRightArrow;", "\xE2\x9F\xBA"},{"DoubleLongRightArrow;", "\xE2\x9F\xB9"},
{"DoubleRightArrow;", "\xE2\x87\x92"},{"DoubleRightTee;", "\xE2\x8A\xA8"},{"DoubleUpArrow;", "\xE2\x87\x91"},{"DoubleUpDownArrow;", "\xE2\x87\x95"},
{"DoubleVerticalBar;", "\xE2\x88\xA5"},{"DownArrow;", "\xE2\x86\x93"},{"DownArrowBar;", "\xE2\xA4\x93"},{"DownArrowUpArrow;", "\xE2\x87\xB5"},
{"DownBreve;", "\xCC\x91"},{"DownLeftRightVector;", "\xE2\xA5\x90"},{"DownLeftTeeVector;", "\xE2\xA5\x9E"},{"DownLeftVector;", "\xE2\x86\xBD"},
{"DownLeftVectorBar;", "\xE2\xA5\x96"},{"DownRightTeeVector;", "\xE2\xA5\x9F"},{"DownRightVector;", "\xE2\x87\x81"},{"DownRightVectorBar;", "\xE2\xA5\x97"},
{"DownTee;", "\xE2\x8A\xA4"},{"DownTeeArrow;", "\xE2\x86\xA7"},{"Downarrow;", "\xE2\x87\x93"},{"Dscr;", "\xF0\x9D\x92\x9F"},
{"Dstrok;", "\xC4\x90"},{"ENG;", "\xC5\x8A"},{"ETH", "\xC3\x90"},{"ETH;", "\xC3\x90"},
{"Eacute", "\xC3\x89"},{"Eacute;", "\xC3\x89"},{"Ecaron;", "\xC4\x9A"},{"Ecirc", "\xC3\x8A"},
{"Ecirc;", "\xC3\x8A"},{"Ecy;", "\xD0\xAD"},{"Edot;", "\xC4\x96"},{"Efr;", "\xF0\x9D\x94\x88"},
{"Egrave", "\xC3\x88"},{"Egrave;", "\xC3\x88"},{"Element;", "\xE2\x88\x88"},{"Emacr;", "\xC4\x92"},
{"EmptySmallSquare;", "\xE2\x97\xBB"},{"EmptyVerySmallSquare;", "\xE2\x96\xAB"},{"Eogon;", "\xC4\x98"},{"Eopf;", "\xF0\x9D\x94\xBC"},
{"Epsilon;", "\xCE\x95"},{"Equal;", "\xE2\xA9\xB5"},{"EqualTilde;", "\xE2\x89\x82"},{"Equilibrium;", "\xE2\x87\x8C"},
{"Escr;", "\xE2\x84\xB0"},{"Esim;", "\xE2\xA9\xB3"},{"Eta;", "\xCE\x97"},{"Euml", "\xC3\x8B"},
{"Euml;", "\xC3\x8B"},{"Exists;", "\xE2\x88\x83"},{"ExponentialE;", "\xE2\x85\x87"},{"Fcy;", "\xD0\xA4"},
{"Ffr;", "\xF0\x9D\x94\x89"},{"FilledSmallSquare;", "\xE2\x97\xBC"},{"FilledVerySmallSquare;", "\xE2\x96\xAA"},{"Fopf;", "\xF0\x9D\x94\xBD"},
{"ForAll;", "\xE2\x88\x80"},{"Fouriertrf;", "\xE2\x84\xB1"},{"Fscr;", "\xE2\x84\xB1"},{"GJcy;", "\xD0\x83"},
{"GT", "\x3E"},{"GT;", "\x3E"},{"Gamma;", "\xCE\x93"},{"Gammad;", "\xCF\x9C"},
{"Gbreve;", "\xC4\x9E"},{"Gcedil;", "\xC4\xA2"},{"Gcirc;", "\xC4\x9C"},{"Gcy;", "\xD0\x93"},
{"Gdot;", "\xC4\xA0"},{"Gfr;", "\xF0\x9D\x94\x8A"},{"Gg;", "\xE2\x8B\x99"},{"Gopf;", "\xF0\x9D\x94\xBE"},
{"GreaterEqual;", "\xE2\x89\xA5"},{"GreaterEqualLess;", "\xE2\x8B\x9B"},{"GreaterFullEqual;", "\xE2\x89\xA7"},{"GreaterGreater;", "\xE2\xAA\xA2"},
{"GreaterLess;", "\xE2\x89\xB7"},{"GreaterSlantEqual;", "\xE2\xA9\xBE"},{"GreaterTilde;", "\xE2\x89\xB3"},{"Gscr;", "\xF0\x9D\x92\xA2"},
{"Gt;", "\xE2\x89\xAB"},{"HARDcy;", "\xD0\xAA"},{"Hacek;", "\xCB\x87"},{"Hat;", "\x5E"},
{"Hcirc;", "\xC4\xA4"},{"Hfr;", "\xE2\x84\x8C"},{"HilbertSpace;", "\xE2\x84\x8B"},{"Hopf;", "\xE2\x84\x8D"},
{"HorizontalLine;", "\xE2\x94\x80"},{"Hscr;", "\xE2\x84\x8B"},{"Hstrok;", "\xC4\xA6"},{"HumpDownHump;", "\xE2\x89\x8E"},
{"HumpEqual;", "\xE2\x89\x8F"},{"IEcy;", "\xD0\x95"},{"IJlig;", "\xC4\xB2"},{"IOcy;", "\xD0\x81"},
{"Iacute", "\xC3\x8D"},{"Iacute;", "\xC3\x8D"},{"Icirc", "\xC3\x8E"},{"Icirc;", "\xC3\x8E"},
{"Icy;", "\xD0\x98"},{"Idot;", "\xC4\xB0"},{"Ifr;", "\xE2\x84\x91"},{"Igrave", "\xC3\x8C"},
{"Igrave;", "\xC3\x8C"},{"Im;", "\xE2\x84\x91"},{"Imacr;", "\xC4\xAA"},{"ImaginaryI;", "\xE2\x85\x88"},
{"Implies;", "\xE2\x87\x92"},{"Int;", "\xE2\x88\xAC"},{"Integral;", "\xE2\x88\xAB"},{"Intersection;", "\xE2\x8B\x82"},
{"InvisibleComma;", "\xE2\x81\xA3"},{"InvisibleTimes;", "\xE2\x81\xA2"},{"Iogon;", "\xC4\xAE"},{"Iopf;", "\xF0\x9D\x95\x80"},
{"Iota;", "\xCE\x99"},{"Iscr;", "\xE2\x84\x90"},{"Itilde;", "\xC4\xA8"},{"Iukcy;", "\xD0\x86"},
{"Iuml", "\xC3\x8F"},{"Iuml;", "\xC3\x8F"},{"Jcirc;", "\xC4\xB4"},{"Jcy;", "\xD0\x99"},
{"Jfr;", "\xF0\x9D\x94\x8D"},{"Jopf;", "\xF0\x9D\x95\x81"},{"Jscr;", "\xF0\x9D\x92\xA5"},{"Jsercy;", "\xD0\x88"},
{"Jukcy;", "\xD0\x84"},{"KHcy;", "\xD0\xA5"},{"KJcy;", "\xD0\x8C"},{"Kappa;", "\xCE\x9A"},
{"Kcedil;", "\xC4\xB6"},{"Kcy;", "\xD0\x9A"},{"Kfr;", "\xF0\x9D\x94\x8E"},{"Kopf;", "\xF0\x9D\x95\x82"},
{"Kscr;", "\xF0\x9D\x92\xA6"},{"LJcy;", "\xD0\x89"},{"LT", "\x3C"},{"LT;", "\x3C"},
{"Lacute;", "\xC4\xB9"},{"Lambda;", "\xCE\x9B"},{"Lang;", "\xE2\x9F\xAA"},{"Laplacetrf;", "\xE2\x84\x92"},
{"Larr;", "\xE2\x86\x9E"},{"Lcaron;", "\xC4\xBD"},{"Lcedil;", "\xC4\xBB"},{"Lcy;", "\xD0\x9B"},
{"LeftAngleBracket;", "\xE2\x9F\xA8"},{"LeftArrow;", "\xE2\x86\x90"},{"LeftArrowBar;", "\xE2\x87\xA4"},{"LeftArrowRightArrow;", "\xE2\x87\x86"},
{"LeftCeiling;", "\xE2\x8C\x88"},{"LeftDoubleBracket;", "\xE2\x9F\xA6"},{"LeftDownTeeVector;", "\xE2\xA5\xA1"},{"LeftDownVector;", "\xE2\x87\x83"},
{"LeftDownVectorBar;", "\xE2\xA5\x99"},{"LeftFloor;", "\xE2\x8C\x8A"},{"LeftRightArrow;", "\xE2\x86\x94"},{"LeftRightVector;", "\xE2\xA5\x8E"},
{"LeftTee;", "\xE2\x8A\xA3"},{"LeftTeeArrow;", "\xE2\x86\xA4"},{"LeftTeeVector;", "\xE2\xA5\x9A"},{"LeftTriangle;", "\xE2\x8A\xB2"},
{"LeftTriangleBar;", "\xE2\xA7\x8F"},{"LeftTriangleEqual;", "\xE2\x8A\xB4"},{"LeftUpDownVector;", "\xE2\xA5\x91"},{"LeftUpTeeVector;", "\xE2\xA5\xA0"},
{"LeftUpVector;", "\xE2\x86\xBF"},{"LeftUpVectorBar;", "\xE2\xA5\x98"},{"LeftVector;", "\xE2\x86\xBC"},{"LeftVectorBar;", "\xE2\xA5\x92"},
{"Leftarrow;", "\xE2\x87\x90"},{"Leftrightarrow;", "\xE2\x87\x94"},{"LessEqualGreater;", "\xE2\x8B\x9A"},{"LessFullEqual;", "\xE2\x89\xA6"},
{"LessGreater;", "\xE2\x89\xB6"},{"LessLess;", "\xE2\xAA\xA1"},{"LessSlantEqual;", "\xE2\xA9\xBD"},{"LessTilde;", "\xE2\x89\xB2"},
{"Lfr;", "\xF0\x9D\x94\x8F"},{"Ll;", "\xE2\x8B\x98"},{"Lleftarrow;", "\xE2\x87\x9A"},{"Lmidot;", "\xC4\xBF"},
{"LongLeftArrow;", "\xE2\x9F\xB5"},{"LongLeftRightArrow;", "\xE2\x9F\xB7"},{"LongRightArrow;", "\xE2\x9F\xB6"},{"Longleftarrow;", "\xE2\x9F\xB8"},
{"Longleftrightarrow;", "\xE2\x9F\xBA"},{"Longrightarrow;", "\xE2\x9F\xB9"},{"Lopf;", "\xF0\x9D\x95\x83"},{"LowerLeftArrow;", "\xE2\x86\x99"},
{"LowerRightArrow;", "\xE2\x86\x98"},{"Lscr;", "\xE2\x84\x92"},{"Lsh;", "\xE2\x86\xB0"},{"Lstrok;", "\xC5\x81"},
{"Lt;", "\xE2\x89\xAA"},{"Map;", "\xE2\xA4\x85"},{"Mcy;", "\xD0\x9C"},{"MediumSpace;", "\xE2\x81\x9F"},
{"Mellintrf;", "\xE2\x84\xB3"},{"Mfr;", "\xF0\x9D\x94\x90"},{"MinusPlus;", "\xE2\x88\x93"},{"Mopf;", "\xF0\x9D\x95\x84"},
{"Mscr;", "\xE2\x84\xB3"},{"Mu;", "\xCE\x9C"},{"NJcy;", "\xD0\x8A"},{"Nacute;", "\xC5\x83"},
{"Ncaron;", "\xC5\x87"},{"Ncedil;", "\xC5\x85"},{"Ncy;", "\xD0\x9D"},{"NegativeMediumSpace;", "\xE2\x80\x8B"},
{"NegativeThickSpace;", "\xE2\x80\x8B"},{"NegativeThinSpace;", "\xE2\x80\x8B"},{"NegativeVeryThinSpace;", "\xE2\x80\x8B"},{"NestedGreaterGreater;", "\xE2\x89\xAB"},
{"NestedLessLess;", "\xE2\x89\xAA"},{"NewLine;", "\x0A"},{"Nfr;", "\xF0\x9D\x94\x91"},{"NoBreak;", "\xE2\x81\xA0"},
{"NonBreakingSpace;", "\xC2\xA0"},{"Nopf;", "\xE2\x84\x95"},{"Not;", "\xE2\xAB\xAC"},{"NotCongruent;", "\xE2\x89\xA2"},
{"NotCupCap;", "\xE2\x89\xAD"},{"NotDoubleVerticalBar;", "\xE2\x88\xA6"},{"NotElement;", "\xE2\x88\x89"},{"NotEqual;", "\xE2\x89\xA0"},
{"NotEqualTilde;", "\xE2\x89\x82\xCC\xB8"},{"NotExists;", "\xE2\x88\x84"},{"NotGreater;", "\xE2\x89\xAF"},{"NotGreaterEqual;", "\xE2\x89\xB1"},
{"NotGreaterFullEqual;", "\xE2\x89\xA7\xCC\xB8"},{"NotGreaterGreater;", "\xE2\x89\xAB\xCC\xB8"},{"NotGreaterLess;", "\xE2\x89\xB9"},{"NotGreaterSlantEqual;", "\xE2\xA9\xBE\xCC\xB8"},
{"NotGreaterTilde;", "\xE2\x89\xB5"},{"NotHumpDownHump;", "\xE2\x89\x8E\xCC\xB8"},{"NotHumpEqual;", "\xE2\x89\x8F\xCC\xB8"},{"NotLeftTriangle;", "\xE2\x8B\xAA"},
{"NotLeftTriangleBar;", "\xE2\xA7\x8F\xCC\xB8"},{"NotLeftTriangleEqual;", "\xE2\x8B\xAC"},{"NotLess;", "\xE2\x89\xAE"},{"NotLessEqual;", "\xE2\x89\xB0"},
{"NotLessGreater;", "\xE2\x89\xB8"},{"NotLessLess;", "\xE2\x89\xAA\xCC\xB8"},{"NotLessSlantEqual;", "\xE2\xA9\xBD\xCC\xB8"},{"NotLessTilde;", "\xE2\x89\xB4"},
{"NotNestedGreaterGreater;", "\xE2\xAA\xA2\xCC\xB8"},{"NotNestedLessLess;", "\xE2\xAA\xA1\xCC\xB8"},{"NotPrecedes;", "\xE2\x8A\x80"},{"NotPrecedesEqual;", "\xE2\xAA\xAF\xCC\xB8"},
{"NotPrecedesSlantEqual;", "\xE2\x8B\xA0"},{"NotReverseElement;", "\xE2\x88\x8C"},{"NotRightTriangle;", "\xE2\x8B\xAB"},{"NotRightTriangleBar;", "\xE2\xA7\x90\xCC\xB8"},
{"NotRightTriangleEqual;", "\xE2\x8B\xAD"},{"NotSquareSubset;", "\xE2\x8A\x8F\xCC\xB8"},{"NotSquareSubsetEqual;", "\xE2\x8B\xA2"},{"NotSquareSuperset;", "\xE2\x8A\x90\xCC\xB8"},
{"NotSquareSupersetEqual;", "\xE2\x8B\xA3"},{"NotSubset;", "\xE2\x8A\x82\xE2\x83\x92"},{"NotSubsetEqual;", "\xE2\x8A\x88"},{"NotSucceeds;", "\xE2\x8A\x81"},
{"NotSucceedsEqual;", "\xE2\xAA\xB0\xCC\xB8"},{"NotSucceedsSlantEqual;", "\xE2\x8B\xA1"},{"NotSucceedsTilde;", "\xE2\x89\xBF\xCC\xB8"},{"NotSuperset;", "\xE2\x8A\x83\xE2\x83\x92"},
{"NotSupersetEqual;", "\xE2\x8A\x89"},{"NotTilde;", "\xE2\x89\x81"},{"NotTildeEqual;", "\xE2\x89\x84"},{"NotTildeFullEqual;", "\xE2\x89\x87"},
{"NotTildeTilde;", "\xE2\x89\x89"},{"NotVerticalBar;", "\xE2\x88\xA4"},{"Nscr;", "\xF0\x9D\x92\xA9"},{"Ntilde", "\xC3\x91"},
{"Ntilde;", "\xC3\x91"},{"Nu;", "\xCE\x9D"},{"OElig;", "\xC5\x92"},{"Oacute", "\xC3\x93"},
{"Oacute;", "\xC3\x93"},{"Ocirc", "\xC3\x94"},{"Ocirc;", "\xC3\x94"},{"Ocy;", "\xD0\x9E"},
{"Odblac;", "\xC5\x90"},{"Ofr;", "\xF0\x9D\x94\x92"},{"Ograve", "\xC3\x92"},{"Ograve;", "\xC3\x92"},
{"Omacr;", "\xC5\x8C"},{"Omega;", "\xCE\xA9"},{"Omicron;", "\xCE\x9F"},{"Oopf;", "\xF0\x9D\x95\x86"},
{"OpenCurlyDoubleQuote;", "\xE2\x80\x9C"},{"OpenCurlyQuote;", "\xE2\x80\x98"},{"Or;", "\xE2\xA9\x94"},{"Oscr;", "\xF0\x9D\x92\xAA"},
{"Oslash", "\xC3\x98"},{"Oslash;", "\xC3\x98"},{"Otilde", "\xC3\x95"},{"Otilde;", "\xC3\x95"},
{"Otimes;", "\xE2\xA8\xB7"},{"Ouml", "\xC3\x96"},{"Ouml;", "\xC3\x96"},{"OverBar;", "\xE2\x80\xBE"},
{"OverBrace;", "\xE2\x8F\x9E"},{"OverBracket;", "\xE2\x8E\xB4"},{"OverParenthesis;", "\xE2\x8F\x9C"},{"PartialD;", "\xE2\x88\x82"},
{"Pcy;", "\xD0\x9F"},{"Pfr;", "\xF0\x9D\x94\x93"},{"Phi;", "\xCE\xA6"},{"Pi;", "\xCE\xA0"},
{"PlusMinus;", "\xC2\xB1"},{"Poincareplane;", "\xE2\x84\x8C"},{"Popf;", "\xE2\x84\x99"},{"Pr;", "\xE2\xAA\xBB"},
{"Precedes;", "\xE2\x89\xBA"},{"PrecedesEqual;", "\xE2\xAA\xAF"},{"PrecedesSlantEqual;", "\xE2\x89\xBC"},{"PrecedesTilde;", "\xE2\x89\xBE"},
{"Prime;", "\xE2\x80\xB3"},{"Product;", "\xE2\x88\x8F"},{"Proportion;", "\xE2\x88\xB7"},{"Proportional;", "\xE2\x88\x9D"},
{"Pscr;", "\xF0\x9D\x92\xAB"},{"Psi;", "\xCE\xA8"},{"QUOT", "\x22"},{"QUOT;", "\x22"},
{"Qfr;", "\xF0\x9D\x94\x94"},{"Qopf;", "\xE2\x84\x9A"},{"Qscr;", "\xF0\x9D\x92\xAC"},{"RBarr;", "\xE2\xA4\x90"},
{"REG", "\xC2\xAE"},{"REG;", "\xC2\xAE"},{"Racute;", "\xC5\x94"},{"Rang;", "\xE2\x9F\xAB"},
{"Rarr;", "\xE2\x86\xA0"},{"Rarrtl;", "\xE2\xA4\x96"},{"Rcaron;", "\xC5\x98"},{"Rcedil;", "\xC5\x96"},
{"Rcy;", "\xD0\xA0"},{"Re;", "\xE2\x84\x9C"},{"ReverseElement;", "\xE2\x88\x8B"},{"ReverseEquilibrium;", "\xE2\x87\x8B"},
{"ReverseUpEquilibrium;", "\xE2\xA5\xAF"},{"Rfr;", "\xE2\x84\x9C"},{"Rho;", "\xCE\xA1"},{"RightAngleBracket;", "\xE2\x9F\xA9"},
{"RightArrow;", "\xE2\x86\x92"},{"RightArrowBar;", "\xE2\x87\xA5"},{"RightArrowLeftArrow;", "\xE2\x87\x84"},{"RightCeiling;", "\xE2\x8C\x89"},
{"RightDoubleBracket;", "\xE2\x9F\xA7"},{"RightDownTeeVector;", "\xE2\xA5\x9D"},{"RightDownVector;", "\xE2\x87\x82"},{"RightDownVectorBar;", "\xE2\xA5\x95"},
{"RightFloor;", "\xE2\x8C\x8B"},{"RightTee;", "\xE2\x8A\xA2"},{"RightTeeArrow;", "\xE2\x86\xA6"},{"RightTeeVector;", "\xE2\xA5\x9B"},
{"RightTriangle;", "\xE2\x8A\xB3"},{"RightTriangleBar;", "\xE2\xA7\x90"},{"RightTriangleEqual;", "\xE2\x8A\xB5"},{"RightUpDownVector;", "\xE2\xA5\x8F"},
{"RightUpTeeVector;", "\xE2\xA5\x9C"},{"RightUpVector;", "\xE2\x86\xBE"},{"RightUpVectorBar;", "\xE2\xA5\x94"},{"RightVector;", "\xE2\x87\x80"},
{"RightVectorBar;", "\xE2\xA5\x93"},{"Rightarrow;", "\xE2\x87\x92"},{"Ropf;", "\xE2\x84\x9D"},{"RoundImplies;", "\xE2\xA5\xB0"},
{"Rrightarrow;", "\xE2\x87\x9B"},{"Rscr;", "\xE2\x84\x9B"},{"Rsh;", "\xE2\x86\xB1"},{"RuleDelayed;", "\xE2\xA7\xB4"},
{"SHCHcy;", "\xD0\xA9"},{"SHcy;", "\xD0\xA8"},{"SOFTcy;", "\xD0\xAC"},{"Sacute;", "\xC5\x9A"},
{"Sc;", "\xE2\xAA\xBC"},{"Scaron;", "\xC5\xA0"},{"Scedil;", "\xC5\x9E"},{"Scirc;", "\xC5\x9C"},
{"Scy;", "\xD0\xA1"},{"Sfr;", "\xF0\x9D\x94\x96"},{"ShortDownArrow;", "\xE2\x86\x93"},{"ShortLeftArrow;", "\xE2\x86\x90"},
{"ShortRightArrow;", "\xE2\x86\x92"},{"ShortUpArrow;", "\xE2\x86\x91"},{"Sigma;", "\xCE\xA3"},{"SmallCircle;", "\xE2\x88\x98"},
{"Sopf;", "\xF0\x9D\x95\x8A"},{"Sqrt;", "\xE2\x88\x9A"},{"Square;", "\xE2\x96\xA1"},{"SquareIntersection;", "\xE2\x8A\x93"},
{"SquareSubset;", "\xE2\x8A\x8F"},{"SquareSubsetEqual;", "\xE2\x8A\x91"},{"SquareSuperset;", "\xE2\x8A\x90"},{"SquareSupersetEqual;", "\xE2\x8A\x92"},
{"SquareUnion;", "\xE2\x8A\x94"},{"Sscr;", "\xF0\x9D\x92\xAE"},{"Star;", "\xE2\x8B\x86"},{"Sub;", "\xE2\x8B\x90"},
{"Subset;", "\xE2\x8B\x90"},{"SubsetEqual;", "\xE2\x8A\x86"},{"Succeeds;", "\xE2\x89\xBB"},{"SucceedsEqual;", "\xE2\xAA\xB0"},
{"SucceedsSlantEqual;", "\xE2\x89\xBD"},{"SucceedsTilde;", "\xE2\x89\xBF"},{"SuchThat;", "\xE2\x88\x8B"},{"Sum;", "\xE2\x88\x91"},
{"Sup;", "\xE2\x8B\x91"},{"Superset;", "\xE2\x8A\x83"},{"SupersetEqual;", "\xE2\x8A\x87"},{"Supset;", "\xE2\x8B\x91"},
{"THORN", "\xC3\x9E"},{"THORN;", "\xC3\x9E"},{"TRADE;", "\xE2\x84\xA2"},{"TSHcy;", "\xD0\x8B"},
{"TScy;", "\xD0\xA6"},{"Tab;", "\x09"},{"Tau;", "\xCE\xA4"},{"Tcaron;", "\xC5\xA4"},
{"Tcedil;", "\xC5\xA2"},{"Tcy;", "\xD0\xA2"},{"Tfr;", "\xF0\x9D\x94\x97"},{"Therefore;", "\xE2\x88\xB4"},
{"Theta;", "\xCE\x98"},{"ThickSpace;", "\xE2\x81\x9F\xE2\x80\x8A"},{"ThinSpace;", "\xE2\x80\x89"},{"Tilde;", "\xE2\x88\xBC"},
{"TildeEqual;", "\xE2\x89\x83"},{"TildeFullEqual;", "\xE2\x89\x85"},{"TildeTilde;", "\xE2\x89\x88"},{"Topf;", "\xF0\x9D\x95\x8B"},
{"TripleDot;", "\xE2\x83\x9B"},{"Tscr;", "\xF0\x9D\x92\xAF"},{"Tstrok;", "\xC5\xA6"},{"Uacute", "\xC3\x9A"},
{"Uacute;", "\xC3\x9A"},{"Uarr;", "\xE2\x86\x9F"},{"Uarrocir;", "\xE2\xA5\x89"},{"Ubrcy;", "\xD0\x8E"},
{"Ubreve;", "\xC5\xAC"},{"Ucirc", "\xC3\x9B"},{"Ucirc;", "\xC3\x9B"},{"Ucy;", "\xD0\xA3"},
{"Udblac;", "\xC5\xB0"},{"Ufr;", "\xF0\x9D\x94\x98"},{"Ugrave", "\xC3\x99"},{"Ugrave;", "\xC3\x99"},
{"Umacr;", "\xC5\xAA"},{"UnderBar;", "\x5F"},{"UnderBrace;", "\xE2\x8F\x9F"},{"UnderBracket;", "\xE2\x8E\xB5"},
{"UnderParenthesis;", "\xE2\x8F\x9D"},{"Union;", "\xE2\x8B\x83"},{"UnionPlus;", "\xE2\x8A\x8E"},{"Uogon;", "\xC5\xB2"},
{"Uopf;", "\xF0\x9D\x95\x8C"},{"UpArrow;", "\xE2\x86\x91"},{"UpArrowBar;", "\xE2\xA4\x92"},{"UpArrowDownArrow;", "\xE2\x87\x85"},
{"UpDownArrow;", "\xE2\x86\x95"},{"UpEquilibrium;", "\xE2\xA5\xAE"},{"UpTee;", "\xE2\x8A\xA5"},{"UpTeeArrow;", "\xE2\x86\xA5"},
{"Uparrow;", "\xE2\x87\x91"},{"Updownarrow;", "\xE2\x87\x95"},{"UpperLeftArrow;", "\xE2\x86\x96"},{"UpperRightArrow;", "\xE2\x86\x97"},
{"Upsi;", "\xCF\x92"},{"Upsilon;", "\xCE\xA5"},{"Uring;", "\xC5\xAE"},{"Uscr;", "\xF0\x9D\x92\xB0"},
{"Utilde;", "\xC5\xA8"},{"Uuml", "\xC3\x9C"},{"Uuml;", "\xC3\x9C"},{"VDash;", "\xE2\x8A\xAB"},
{"Vbar;", "\xE2\xAB\xAB"},{"Vcy;", "\xD0\x92"},{"Vdash;", "\xE2\x8A\xA9"},{"Vdashl;", "\xE2\xAB\xA6"},
{"Vee;", "\xE2\x8B\x81"},{"Verbar;", "\xE2\x80\x96"},{"Vert;", "\xE2\x80\x96"},{"VerticalBar;", "\xE2\x88\xA3"},
{"VerticalLine;", "\x7C"},{"VerticalSeparator;", "\xE2\x9D\x98"},{"VerticalTilde;", "\xE2\x89\x80"},{"VeryThinSpace;", "\xE2\x80\x8A"},
{"Vfr;", "\xF0\x9D\x94\x99"},{"Vopf;", "\xF0\x9D\x95\x8D"},{"Vscr;", "\xF0\x9D\x92\xB1"},{"Vvdash;", "\xE2\x8A\xAA"},
{"Wcirc;", "\xC5\xB4"},{"Wedge;", "\xE2\x8B\x80"},{"Wfr;", "\xF0\x9D\x94\x9A"},{"Wopf;", "\xF0\x9D\x95\x8E"},
{"Wscr;", "\xF0\x9D\x92\xB2"},{"Xfr;", "\xF0\x9D\x94\x9B"},{"Xi;", "\xCE\x9E"},{"Xopf;", "\xF0\x9D\x95\x8F"},
{"Xscr;", "\xF0\x9D\x92\xB3"},{"YAcy;", "\xD0\xAF"},{"YIcy;", "\xD0\x87"},{"YUcy;", "\xD0\xAE"},
{"Yacute", "\xC3\x9D"},{"Yacute;", "\xC3\x9D"},{"Ycirc;", "\xC5\xB6"},{"Ycy;", "\xD0\xAB"},
{"Yfr;", "\xF0\x9D\x94\x9C"},{"Yopf;", "\xF0\x9D\x95\x90"},{"Yscr;", "\xF0\x9D\x92\xB4"},{"Yuml;", "\xC5\xB8"},
{"ZHcy;", "\xD0\x96"},{"Zacute;", "\xC5\xB9"},{"Zcaron;", "\xC5\xBD"},{"Zcy;", "\xD0\x97"},
{"Zdot;", "\xC5\xBB"},{"ZeroWidthSpace;", "\xE2\x80\x8B"},{"Zeta;", "\xCE\x96"},{"Zfr;", "\xE2\x84\xA8"},
{"Zopf;", "\xE2\x84\xA4"},{"Zscr;", "\xF0\x9D\x92\xB5"},{"aacute", "\xC3\xA1"},{"aacute;", "\xC3\xA1"},
{"abreve;", "\xC4\x83"},{"ac;", "\xE2\x88\xBE"},{"acE;", "\xE2\x88\xBE\xCC\xB3"},{"acd;", "\xE2\x88\xBF"},
{"acirc", "\xC3\xA2"},{"acirc;", "\xC3\xA2"},{"acute", "\xC2\xB4"},{"acute;", "\xC2\xB4"},
{"acy;", "\xD0\xB0"},{"aelig", "\xC3\xA6"},{"aelig;", "\xC3\xA6"},{"af;", "\xE2\x81\xA1"},
{"afr;", "\xF0\x9D\x94\x9E"},{"agrave", "\xC3\xA0"},{"agrave;", "\xC3\xA0"},{"alefsym;", "\xE2\x84\xB5"},
{"aleph;", "\xE2\x84\xB5"},{"alpha;", "\xCE\xB1"},{"amacr;", "\xC4\x81"},{"amalg;", "\xE2\xA8\xBF"},
{"amp", "\x26"},{"amp;", "\x26"},{"and;", "\xE2\x88\xA7"},{"andand;", "\xE2\xA9\x95"},
{"andd;", "\xE2\xA9\x9C"},{"andslope;", "\xE2\xA9\x98"},{"andv;", "\xE2\xA9\x9A"},{"ang;", "\xE2\x88\xA0"},
{"ange;", "\xE2\xA6\xA4"},{"angle;", "\xE2\x88\xA0"},{"angmsd;", "\xE2\x88\xA1"},{"angmsdaa;", "\xE2\xA6\xA8"},
{"angmsdab;", "\xE2\xA6\xA9"},{"angmsdac;", "\xE2\xA6\xAA"},{"angmsdad;", "\xE2\xA6\xAB"},{"angmsdae;", "\xE2\xA6\xAC"},
{"angmsdaf;", "\xE2\xA6\xAD"},{"angmsdag;", "\xE2\xA6\xAE"},{"angmsdah;", "\xE2\xA6\xAF"},{"angrt;", "\xE2\x88\x9F"},
{"angrtvb;", "\xE2\x8A\xBE"},{"angrtvbd;", "\xE2\xA6\x9D"},{"angsph;", "\xE2\x88\xA2"},{"angst;", "\xC3\x85"},
{"angzarr;", "\xE2\x8D\xBC"},{"aogon;", "\xC4\x85"},{"aopf;", "\xF0\x9D\x95\x92"},{"ap;", "\xE2\x89\x88"},
{"apE;", "\xE2\xA9\xB0"},{"apacir;", "\xE2\xA9\xAF"},{"ape;", "\xE2\x89\x8A"},{"apid;", "\xE2\x89\x8B"},
{"apos;", "\x27"},{"approx;", "\xE2\x89\x88"},{"approxeq;", "\xE2\x89\x8A"},{"aring", "\xC3\xA5"},
{"aring;", "\xC3\xA5"},{"ascr;", "\xF0\x9D\x92\xB6"},{"ast;", "\x2A"},{"asymp;", "\xE2\x89\x88"},
{"asympeq;", "\xE2\x89\x8D"},{"atilde", "\xC3\xA3"},{"atilde;", "\xC3\xA3"},{"auml", "\xC3\xA4"},
{"auml;", "\xC3\xA4"},{"awconint;", "\xE2\x88\xB3"},{"awint;", "\xE2\xA8\x91"},{"bNot;", "\xE2\xAB\xAD"},
{"backcong;", "\xE2\x89\x8C"},{"backepsilon;", "\xCF\xB6"},{"backprime;", "\xE2\x80\xB5"},{"backsim;", "\xE2\x88\xBD"},
{"backsimeq;", "\xE2\x8B\x8D"},{"barvee;", "\xE2\x8A\xBD"},{"barwed;", "\xE2\x8C\x85"},{"barwedge;", "\xE2\x8C\x85"},
{"bbrk;", "\xE2\x8E\xB5"},{"bbrktbrk;", "\xE2\x8E\xB6"},{"bcong;", "\xE2\x89\x8C"},{"bcy;", "\xD0\xB1"},
{"bdquo;", "\xE2\x80\x9E"},{"becaus;", "\xE2\x88\xB5"},{"because;", "\xE2\x88\xB5"},{"bemptyv;", "\xE2\xA6\xB0"},
{"bepsi;", "\xCF\xB6"},{"bernou;", "\xE2\x84\xAC"},{"beta;", "\xCE\xB2"},{"beth;", "\xE2\x84\xB6"},
{"between;", "\xE2\x89\xAC"},{"bfr;", "\xF0\x9D\x94\x9F"},{"bigcap;", "\xE2\x8B\x82"},{"bigcirc;", "\xE2\x97\xAF"},
{"bigcup;", "\xE2\x8B\x83"},{"bigodot;", "\xE2\xA8\x80"},{"bigoplus;", "\xE2\xA8\x81"},{"bigotimes;", "\xE2\xA8\x82"},
{"bigsqcup;", "\xE2\xA8\x86"},{"bigstar;", "\xE2\x98\x85"},{"bigtriangledown;", "\xE2\x96\xBD"},{"bigtriangleup;", "\xE2\x96\xB3"},
{"biguplus;", "\xE2\xA8\x84"},{"bigvee;", "\xE2\x8B\x81"},{"bigwedge;", "\xE2\x8B\x80"},{"bkarow;", "\xE2\xA4\x8D"},
{"blacklozenge;", "\xE2\xA7\xAB"},{"blacksquare;", "\xE2\x96\xAA"},{"blacktriangle;", "\xE2\x96\xB4"},{"blacktriangledown;", "\xE2\x96\xBE"},
{"blacktriangleleft;", "\xE2\x97\x82"},{"blacktriangleright;", "\xE2\x96\xB8"},{"blank;", "\xE2\x90\xA3"},{"blk12;", "\xE2\x96\x92"},
{"blk14;", "\xE2\x96\x91"},{"blk34;", "\xE2\x96\x93"},{"block;", "\xE2\x96\x88"},{"bne;", "\x3D\xE2\x83\xA5"},
{"bnequiv;", "\xE2\x89\xA1\xE2\x83\xA5"},{"bnot;", "\xE2\x8C\x90"},{"bopf;", "\xF0\x9D\x95\x93"},{"bot;", "\xE2\x8A\xA5"},
{"bottom;", "\xE2\x8A\xA5"},{"bowtie;", "\xE2\x8B\x88"},{"boxDL;", "\xE2\x95\x97"},{"boxDR;", "\xE2\x95\x94"},
{"boxDl;", "\xE2\x95\x96"},{"boxDr;", "\xE2\x95\x93"},{"boxH;", "\xE2\x95\x90"},{"boxHD;", "\xE2\x95\xA6"},
{"boxHU;", "\xE2\x95\xA9"},{"boxHd;", "\xE2\x95\xA4"},{"boxHu;", "\xE2\x95\xA7"},{"boxUL;", "\xE2\x95\x9D"},
{"boxUR;", "\xE2\x95\x9A"},{"boxUl;", "\xE2\x95\x9C"},{"boxUr;", "\xE2\x95\x99"},{"boxV;", "\xE2\x95\x91"},
{"boxVH;", "\xE2\x95\xAC"},{"boxVL;", "\xE2\x95\xA3"},{"boxVR;", "\xE2\x95\xA0"},{"boxVh;", "\xE2\x95\xAB"},
{"boxVl;", "\xE2\x95\xA2"},{"boxVr;", "\xE2\x95\x9F"},{"boxbox;", "\xE2\xA7\x89"},{"boxdL;", "\xE2\x95\x95"},
{"boxdR;", "\xE2\x95\x92"},{"boxdl;", "\xE2\x94\x90"},{"boxdr;", "\xE2\x94\x8C"},{"boxh;", "\xE2\x94\x80"},
{"boxhD;", "\xE2\x95\xA5"},{"boxhU;", "\xE2\x95\xA8"},{"boxhd;", "\xE2\x94\xAC"},{"boxhu;", "\xE2\x94\xB4"},
{"boxminus;", "\xE2\x8A\x9F"},{"boxplus;", "\xE2\x8A\x9E"},{"boxtimes;", "\xE2\x8A\xA0"},{"boxuL;", "\xE2\x95\x9B"},
{"boxuR;", "\xE2\x95\x98"},{"boxul;", "\xE2\x94\x98"},{"boxur;", "\xE2\x94\x94"},{"boxv;", "\xE2\x94\x82"},
{"boxvH;", "\xE2\x95\xAA"},{"boxvL;", "\xE2\x95\xA1"},{"boxvR;", "\xE2\x95\x9E"},{"boxvh;", "\xE2\x94\xBC"},
{"boxvl;", "\xE2\x94\xA4"},{"boxvr;", "\xE2\x94\x9C"},{"bprime;", "\xE2\x80\xB5"},{"breve;", "\xCB\x98"},
{"brvbar", "\xC2\xA6"},{"brvbar;", "\xC2\xA6"},{"bscr;", "\xF0\x9D\x92\xB7"},{"bsemi;", "\xE2\x81\x8F"},
{"bsim;", "\xE2\x88\xBD"},{"bsime;", "\xE2\x8B\x8D"},{"bsol;", "\x5C"},{"bsolb;", "\xE2\xA7\x85"},
{"bsolhsub;", "\xE2\x9F\x88"},{"bull;", "\xE2\x80\xA2"},{"bullet;", "\xE2\x80\xA2"},{"bump;", "\xE2\x89\x8E"},
{"bumpE;", "\xE2\xAA\xAE"},{"bumpe;", "\xE2\x89\x8F"},{"bumpeq;", "\xE2\x89\x8F"},{"cacute;", "\xC4\x87"},
{"cap;", "\xE2\x88\xA9"},{"capand;", "\xE2\xA9\x84"},{"capbrcup;", "\xE2\xA9\x89"},{"capcap;", "\xE2\xA9\x8B"},
{"capcup;", "\xE2\xA9\x87"},{"capdot;", "\xE2\xA9\x80"},{"caps;", "\xE2\x88\xA9\xEF\xB8\x80"},{"caret;", "\xE2\x81\x81"},
{"caron;", "\xCB\x87"},{"ccaps;", "\xE2\xA9\x8D"},{"ccaron;", "\xC4\x8D"},{"ccedil", "\xC3\xA7"},
{"ccedil;", "\xC3\xA7"},{"ccirc;", "\xC4\x89"},{"ccups;", "\xE2\xA9\x8C"},{"ccupssm;", "\xE2\xA9\x90"},
{"cdot;", "\xC4\x8B"},{"cedil", "\xC2\xB8"},{"cedil;", "\xC2\xB8"},{"cemptyv;", "\xE2\xA6\xB2"},
{"cent", "\xC2\xA2"},{"cent;", "\xC2\xA2"},{"centerdot;", "\xC2\xB7"},{"cfr;", "\xF0\x9D\x94\xA0"},
{"chcy;", "\xD1\x87"},{"check;", "\xE2\x9C\x93"},{"checkmark;", "\xE2\x9C\x93"},{"chi;", "\xCF\x87"},
{"cir;", "\xE2\x97\x8B"},{"cirE;", "\xE2\xA7\x83"},{"circ;", "\xCB\x86"},{"circeq;", "\xE2\x89\x97"},
{"circlearrowleft;", "\xE2\x86\xBA"},{"circlearrowright;", "\xE2\x86\xBB"},{"circledR;", "\xC2\xAE"},{"circledS;", "\xE2\x93\x88"},
{"circledast;", "\xE2\x8A\x9B"},{"circledcirc;", "\xE2\x8A\x9A"},{"circleddash;", "\xE2\x8A\x9D"},{"cire;", "\xE2\x89\x97"},
{"cirfnint;", "\xE2\xA8\x90"},{"cirmid;", "\xE2\xAB\xAF"},{"cirscir;", "\xE2\xA7\x82"},{"clubs;", "\xE2\x99\xA3"},
{"clubsuit;", "\xE2\x99\xA3"},{"colon;", "\x3A"},{"colone;", "\xE2\x89\x94"},{"coloneq;", "\xE2\x89\x94"},
{"comma;", "\x2C"},{"commat;", "\x40"},{"comp;", "\xE2\x88\x81"},{"compfn;", "\xE2\x88\x98"},
{"complement;", "\xE2\x88\x81"},{"complexes;", "\xE2\x84\x82"},{"cong;", "\xE2\x89\x85"},{"congdot;", "\xE2\xA9\xAD"},
{"conint;", "\xE2\x88\xAE"},{"copf;", "\xF0\x9D\x95\x94"},{"coprod;", "\xE2\x88\x90"},{"copy", "\xC2\xA9"},
{"copy;", "\xC2\xA9"},{"copysr;", "\xE2\x84\x97"},{"crarr;", "\xE2\x86\xB5"},{"cross;", "\xE2\x9C\x97"},
{"cscr;", "\xF0\x9D\x92\xB8"},{"csub;", "\xE2\xAB\x8F"},{"csube;", "\xE2\xAB\x91"},{"csup;", "\xE2\xAB\x90"},
{"csupe;", "\xE2\xAB\x92"},{"ctdot;", "\xE2\x8B\xAF"},{"cudarrl;", "\xE2\xA4\xB8"},{"cudarrr;", "\xE2\xA4\xB5"},
{"cuepr;", "\xE2\x8B\x9E"},{"cuesc;", "\xE2\x8B\x9F"},{"cularr;", "\xE2\x86\xB6"},{"cularrp;", "\xE2\xA4\xBD"},
{"cup;", "\xE2\x88\xAA"},{"cupbrcap;", "\xE2\xA9\x88"},{"cupcap;", "\xE2\xA9\x86"},{"cupcup;", "\xE2\xA9\x8A"},
{"cupdot;", "\xE2\x8A\x8D"},{"cupor;", "\xE2\xA9\x85"},{"cups;", "\xE2\x88\xAA\xEF\xB8\x80"},{"curarr;", "\xE2\x86\xB7"},
{"curarrm;", "\xE2\xA4\xBC"},{"curlyeqprec;", "\xE2\x8B\x9E"},{"curlyeqsucc;", "\xE2\x8B\x9F"},{"curlyvee;", "\xE2\x8B\x8E"},
{"curlywedge;", "\xE2\x8B\x8F"},{"curren", "\xC2\xA4"},{"curren;", "\xC2\xA4"},{"curvearrowleft;", "\xE2\x86\xB6"},
{"curvearrowright;", "\xE2\x86\xB7"},{"cuvee;", "\xE2\x8B\x8E"},{"cuwed;", "\xE2\x8B\x8F"},{"cwconint;", "\xE2\x88\xB2"},
{"cwint;", "\xE2\x88\xB1"},{"cylcty;", "\xE2\x8C\xAD"},{"dArr;", "\xE2\x87\x93"},{"dHar;", "\xE2\xA5\xA5"},
{"dagger;", "\xE2\x80\xA0"},{"daleth;", "\xE2\x84\xB8"},{"darr;", "\xE2\x86\x93"},{"dash;", "\xE2\x80\x90"},
{"dashv;", "\xE2\x8A\xA3"},{"dbkarow;", "\xE2\xA4\x8F"},{"dblac;", "\xCB\x9D"},{"dcaron;", "\xC4\x8F"},
{"dcy;", "\xD0\xB4"},{"dd;", "\xE2\x85\x86"},{"ddagger;", "\xE2\x80\xA1"},{"ddarr;", "\xE2\x87\x8A"},
{"ddotseq;", "\xE2\xA9\xB7"},{"deg", "\xC2\xB0"},{"deg;", "\xC2\xB0"},{"delta;", "\xCE\xB4"},
{"demptyv;", "\xE2\xA6\xB1"},{"dfisht;", "\xE2\xA5\xBF"},{"dfr;", "\xF0\x9D\x94\xA1"},{"dharl;", "\xE2\x87\x83"},
{"dharr;", "\xE2\x87\x82"},{"diam;", "\xE2\x8B\x84"},{"diamond;", "\xE2\x8B\x84"},{"diamondsuit;", "\xE2\x99\xA6"},
{"diams;", "\xE2\x99\xA6"},{"die;", "\xC2\xA8"},{"digamma;", "\xCF\x9D"},{"disin;", "\xE2\x8B\xB2"},
{"div;", "\xC3\xB7"},{"divide", "\xC3\xB7"},{"divide;", "\xC3\xB7"},{"divideontimes;", "\xE2\x8B\x87"},
{"divonx;", "\xE2\x8B\x87"},{"djcy;", "\xD1\x92"},{"dlcorn;", "\xE2\x8C\x9E"},{"dlcrop;", "\xE2\x8C\x8D"},
{"dollar;", "\x24"},{"dopf;", "\xF0\x9D\x95\x95"},{"dot;", "\xCB\x99"},{"doteq;", "\xE2\x89\x90"},
{"doteqdot;", "\xE2\x89\x91"},{"dotminus;", "\xE2\x88\xB8"},{"dotplus;", "\xE2\x88\x94"},{"dotsquare;", "\xE2\x8A\xA1"},
{"doublebarwedge;", "\xE2\x8C\x86"},{"downarrow;", "\xE2\x86\x93"},{"downdownarrows;", "\xE2\x87\x8A"},{"downharpoonleft;", "\xE2\x87\x83"},
{"downharpoonright;", "\xE2\x87\x82"},{"drbkarow;", "\xE2\xA4\x90"},{"drcorn;", "\xE2\x8C\x9F"},{"drcrop;", "\xE2\x8C\x8C"},
{"dscr;", "\xF0\x9D\x92\xB9"},{"dscy;", "\xD1\x95"},{"dsol;", "\xE2\xA7\xB6"},{"dstrok;", "\xC4\x91"},
{"dtdot;", "\xE2\x8B\xB1"},{"dtri;", "\xE2\x96\xBF"},{"dtrif;", "\xE2\x96\xBE"},{"duarr;", "\xE2\x87\xB5"},
{"duhar;", "\xE2\xA5\xAF"},{"dwangle;", "\xE2\xA6\xA6"},{"dzcy;", "\xD1\x9F"},{"dzigrarr;", "\xE2\x9F\xBF"},
{"eDDot;", "\xE2\xA9\xB7"},{"eDot;", "\xE2\x89\x91"},{"eacute", "\xC3\xA9"},{"eacute;", "\xC3\xA9"},
{"easter;", "\xE2\xA9\xAE"},{"ecaron;", "\xC4\x9B"},{"ecir;", "\xE2\x89\x96"},{"ecirc", "\xC3\xAA"},
{"ecirc;", "\xC3\xAA"},{"ecolon;", "\xE2\x89\x95"},{"ecy;", "\xD1\x8D"},{"edot;", "\xC4\x97"},
{"ee;", "\xE2\x85\x87"},{"efDot;", "\xE2\x89\x92"},{"efr;", "\xF0\x9D\x94\xA2"},{"eg;", "\xE2\xAA\x9A"},
{"egrave", "\xC3\xA8"},{"egrave;", "\xC3\xA8"},{"egs;", "\xE2\xAA\x96"},{"egsdot;", "\xE2\xAA\x98"},
{"el;", "\xE2\xAA\x99"},{"elinters;", "\xE2\x8F\xA7"},{"ell;", "\xE2\x84\x93"},{"els;", "\xE2\xAA\x95"},
{"elsdot;", "\xE2\xAA\x97"},{"emacr;", "\xC4\x93"},{"empty;", "\xE2\x88\x85"},{"emptyset;", "\xE2\x88\x85"},
{"emptyv;", "\xE2\x88\x85"},{"emsp13;", "\xE2\x80\x84"},{"emsp14;", "\xE2\x80\x85"},{"emsp;", "\xE2\x80\x83"},
{"eng;", "\xC5\x8B"},{"ensp;", "\xE2\x80\x82"},{"eogon;", "\xC4\x99"},{"eopf;", "\xF0\x9D\x95\x96"},
{"epar;", "\xE2\x8B\x95"},{"eparsl;", "\xE2\xA7\xA3"},{"eplus;", "\xE2\xA9\xB1"},{"epsi;", "\xCE\xB5"},
{"epsilon;", "\xCE\xB5"},{"epsiv;", "\xCF\xB5"},{"eqcirc;", "\xE2\x89\x96"},{"eqcolon;", "\xE2\x89\x95"},
{"eqsim;", "\xE2\x89\x82"},{"eqslantgtr;", "\xE2\xAA\x96"},{"eqslantless;", "\xE2\xAA\x95"},{"equals;", "\x3D"},
{"equest;", "\xE2\x89\x9F"},{"equiv;", "\xE2\x89\xA1"},{"equivDD;", "\xE2\xA9\xB8"},{"eqvparsl;", "\xE2\xA7\xA5"},
{"erDot;", "\xE2\x89\x93"},{"erarr;", "\xE2\xA5\xB1"},{"escr;", "\xE2\x84\xAF"},{"esdot;", "\xE2\x89\x90"},
{"esim;", "\xE2\x89\x82"},{"eta;", "\xCE\xB7"},{"eth", "\xC3\xB0"},{"eth;", "\xC3\xB0"},
{"euml", "\xC3\xAB"},{"euml;", "\xC3\xAB"},{"euro;", "\xE2\x82\xAC"},{"excl;", "\x21"},
{"exist;", "\xE2\x88\x83"},{"expectation;", "\xE2\x84\xB0"},{"exponentiale;", "\xE2\x85\x87"},{"fallingdotseq;", "\xE2\x89\x92"},
{"fcy;", "\xD1\x84"},{"female;", "\xE2\x99\x80"},{"ffilig;", "\xEF\xAC\x83"},{"fflig;", "\xEF\xAC\x80"},
{"ffllig;", "\xEF\xAC\x84"},{"ffr;", "\xF0\x9D\x94\xA3"},{"filig;", "\xEF\xAC\x81"},{"fjlig;", "\x66\x6A"},
{"flat;", "\xE2\x99\xAD"},{"fllig;", "\xEF\xAC\x82"},{"fltns;", "\xE2\x96\xB1"},{"fnof;", "\xC6\x92"},
{"fopf;", "\xF0\x9D\x95\x97"},{"forall;", "\xE2\x88\x80"},{"fork;", "\xE2\x8B\x94"},{"forkv;", "\xE2\xAB\x99"},
{"fpartint;", "\xE2\xA8\x8D"},{"frac12", "\xC2\xBD"},{"frac12;", "\xC2\xBD"},{"frac13;", "\xE2\x85\x93"},
{"frac14", "\xC2\xBC"},{"frac14;", "\xC2\xBC"},{"frac15;", "\xE2\x85\x95"},{"frac16;", "\xE2\x85\x99"},
{"frac18;", "\xE2\x85\x9B"},{"frac23;", "\xE2\x85\x94"},{"frac25;", "\xE2\x85\x96"},{"frac34", "\xC2\xBE"},
{"frac34;", "\xC2\xBE"},{"frac35;", "\xE2\x85\x97"},{"frac38;", "\xE2\x85\x9C"},{"frac45;", "\xE2\x85\x98"},
{"frac56;", "\xE2\x85\x9A"},{"frac58;", "\xE2\x85\x9D"},{"frac78;", "\xE2\x85\x9E"},{"frasl;", "\xE2\x81\x84"},
{"frown;", "\xE2\x8C\xA2"},{"fscr;", "\xF0\x9D\x92\xBB"},{"gE;", "\xE2\x89\xA7"},{"gEl;", "\xE2\xAA\x8C"},
{"gacute;", "\xC7\xB5"},{"gamma;", "\xCE\xB3"},{"gammad;", "\xCF\x9D"},{"gap;", "\xE2\xAA\x86"},
{"gbreve;", "\xC4\x9F"},{"gcirc;", "\xC4\x9D"},{"gcy;", "\xD0\xB3"},{"gdot;", "\xC4\xA1"},
{"ge;", "\xE2\x89\xA5"},{"gel;", "\xE2\x8B\x9B"},{"geq;", "\xE2\x89\xA5"},{"geqq;", "\xE2\x89\xA7"},
{"geqslant;", "\xE2\xA9\xBE"},{"ges;", "\xE2\xA9\xBE"},{"gescc;", "\xE2\xAA\xA9"},{"gesdot;", "\xE2\xAA\x80"},
{"gesdoto;", "\xE2\xAA\x82"},{"gesdotol;", "\xE2\xAA\x84"},{"gesl;", "\xE2\x8B\x9B\xEF\xB8\x80"},{"gesles;", "\xE2\xAA\x94"},
{"gfr;", "\xF0\x9D\x94\xA4"},{"gg;", "\xE2\x89\xAB"},{"ggg;", "\xE2\x8B\x99"},{"gimel;", "\xE2\x84\xB7"},
{"gjcy;", "\xD1\x93"},{"gl;", "\xE2\x89\xB7"},{"glE;", "\xE2\xAA\x92"},{"gla;", "\xE2\xAA\xA5"},
{"glj;", "\xE2\xAA\xA4"},{"gnE;", "\xE2\x89\xA9"},{"gnap;", "\xE2\xAA\x8A"},{"gnapprox;", "\xE2\xAA\x8A"},
{"gne;", "\xE2\xAA\x88"},{"gneq;", "\xE2\xAA\x88"},{"gneqq;", "\xE2\x89\xA9"},{"gnsim;", "\xE2\x8B\xA7"},
{"gopf;", "\xF0\x9D\x95\x98"},{"grave;", "\x60"},{"gscr;", "\xE2\x84\x8A"},{"gsim;", "\xE2\x89\xB3"},
{"gsime;", "\xE2\xAA\x8E"},{"gsiml;", "\xE2\xAA\x90"},{"gt", "\x3E"},{"gt;", "\x3E"},
{"gtcc;", "\xE2\xAA\xA7"},{"gtcir;", "\xE2\xA9\xBA"},{"gtdot;", "\xE2\x8B\x97"},{"gtlPar;", "\xE2\xA6\x95"},
{"gtquest;", "\xE2\xA9\xBC"},{"gtrapprox;", "\xE2\xAA\x86"},{"gtrarr;", "\xE2\xA5\xB8"},{"gtrdot;", "\xE2\x8B\x97"},
{"gtreqless;", "\xE2\x8B\x9B"},{"gtreqqless;", "\xE2\xAA\x8C"},{"gtrless;", "\xE2\x89\xB7"},{"gtrsim;", "\xE2\x89\xB3"},
{"gvertneqq;", "\xE2\x89\xA9\xEF\xB8\x80"},{"gvnE;", "\xE2\x89\xA9\xEF\xB8\x80"},{"hArr;", "\xE2\x87\x94"},{"hairsp;", "\xE2\x80\x8A"},
{"half;", "\xC2\xBD"},{"hamilt;", "\xE2\x84\x8B"},{"hardcy;", "\xD1\x8A"},{"harr;", "\xE2\x86\x94"},
{"harrcir;", "\xE2\xA5\x88"},{"harrw;", "\xE2\x86\xAD"},{"hbar;", "\xE2\x84\x8F"},{"hcirc;", "\xC4\xA5"},
{"hearts;", "\xE2\x99\xA5"},{"heartsuit;", "\xE2\x99\xA5"},{"hellip;", "\xE2\x80\xA6"},{"hercon;", "\xE2\x8A\xB9"},
{"hfr;", "\xF0\x9D\x94\xA5"},{"hksearow;", "\xE2\xA4\xA5"},{"hkswarow;", "\xE2\xA4\xA6"},{"hoarr;", "\xE2\x87\xBF"},
{"homtht;", "\xE2\x88\xBB"},{"hookleftarrow;", "\xE2\x86\xA9"},{"hookrightarrow;", "\xE2\x86\xAA"},{"hopf;", "\xF0\x9D\x95\x99"},
{"horbar;", "\xE2\x80\x95"},{"hscr;", "\xF0\x9D\x92\xBD"},{"hslash;", "\xE2\x84\x8F"},{"hstrok;", "\xC4\xA7"},
{"hybull;", "\xE2\x81\x83"},{"hyphen;", "\xE2\x80\x90"},{"iacute", "\xC3\xAD"},{"iacute;", "\xC3\xAD"},
{"ic;", "\xE2\x81\xA3"},{"icirc", "\xC3\xAE"},{"icirc;", "\xC3\xAE"},{"icy;", "\xD0\xB8"},
{"iecy;", "\xD0\xB5"},{"iexcl", "\xC2\xA1"},{"iexcl;", "\xC2\xA1"},{"iff;", "\xE2\x87\x94"},
{"ifr;", "\xF0\x9D\x94\xA6"},{"igrave", "\xC3\xAC"},{"igrave;", "\xC3\xAC"},{"ii;", "\xE2\x85\x88"},
{"iiiint;", "\xE2\xA8\x8C"},{"iiint;", "\xE2\x88\xAD"},{"iinfin;", "\xE2\xA7\x9C"},{"iiota;", "\xE2\x84\xA9"},
{"ijlig;", "\xC4\xB3"},{"imacr;", "\xC4\xAB"},{"image;", "\xE2\x84\x91"},{"imagline;", "\xE2\x84\x90"},
{"imagpart;", "\xE2\x84\x91"},{"imath;", "\xC4\xB1"},{"imof;", "\xE2\x8A\xB7"},{"imped;", "\xC6\xB5"},
{"in;", "\xE2\x88\x88"},{"incare;", "\xE2\x84\x85"},{"infin;", "\xE2\x88\x9E"},{"infintie;", "\xE2\xA7\x9D"},
{"inodot;", "\xC4\xB1"},{"int;", "\xE2\x88\xAB"},{"intcal;", "\xE2\x8A\xBA"},{"integers;", "\xE2\x84\xA4"},
{"intercal;", "\xE2\x8A\xBA"},{"intlarhk;", "\xE2\xA8\x97"},{"intprod;", "\xE2\xA8\xBC"},{"iocy;", "\xD1\x91"},
{"iogon;", "\xC4\xAF"},{"iopf;", "\xF0\x9D\x95\x9A"},{"iota;", "\xCE\xB9"},{"iprod;", "\xE2\xA8\xBC"},
{"iquest", "\xC2\xBF"},{"iquest;", "\xC2\xBF"},{"iscr;", "\xF0\x9D\x92\xBE"},{"isin;", "\xE2\x88\x88"},
{"isinE;", "\xE2\x8B\xB9"},{"isindot;", "\xE2\x8B\xB5"},{"isins;", "\xE2\x8B\xB4"},{"isinsv;", "\xE2\x8B\xB3"},
{"isinv;", "\xE2\x88\x88"},{"it;", "\xE2\x81\xA2"},{"itilde;", "\xC4\xA9"},{"iukcy;", "\xD1\x96"},
{"iuml", "\xC3\xAF"},{"iuml;", "\xC3\xAF"},{"jcirc;", "\xC4\xB5"},{"jcy;", "\xD0\xB9"},
{"jfr;", "\xF0\x9D\x94\xA7"},{"jmath;", "\xC8\xB7"},{"jopf;", "\xF0\x9D\x95\x9B"},{"jscr;", "\xF0\x9D\x92\xBF"},
{"jsercy;", "\xD1\x98"},{"jukcy;", "\xD1\x94"},{"kappa;", "\xCE\xBA"},{"kappav;", "\xCF\xB0"},
{"kcedil;", "\xC4\xB7"},{"kcy;", "\xD0\xBA"},{"kfr;", "\xF0\x9D\x94\xA8"},{"kgreen;", "\xC4\xB8"},
{"khcy;", "\xD1\x85"},{"kjcy;", "\xD1\x9C"},{"kopf;", "\xF0\x9D\x95\x9C"},{"kscr;", "\xF0\x9D\x93\x80"},
{"lAarr;", "\xE2\x87\x9A"},{"lArr;", "\xE2\x87\x90"},{"lAtail;", "\xE2\xA4\x9B"},{"lBarr;", "\xE2\xA4\x8E"},
{"lE;", "\xE2\x89\xA6"},{"lEg;", "\xE2\xAA\x8B"},{"lHar;", "\xE2\xA5\xA2"},{"lacute;", "\xC4\xBA"},
{"laemptyv;", "\xE2\xA6\xB4"},{"lagran;", "\xE2\x84\x92"},{"lambda;", "\xCE\xBB"},{"lang;", "\xE2\x9F\xA8"},
{"langd;", "\xE2\xA6\x91"},{"langle;", "\xE2\x9F\xA8"},{"lap;", "\xE2\xAA\x85"},{"laquo", "\xC2\xAB"},
{"laquo;", "\xC2\xAB"},{"larr;", "\xE2\x86\x90"},{"larrb;", "\xE2\x87\xA4"},{"larrbfs;", "\xE2\xA4\x9F"},
{"larrfs;", "\xE2\xA4\x9D"},{"larrhk;", "\xE2\x86\xA9"},{"larrlp;", "\xE2\x86\xAB"},{"larrpl;", "\xE2\xA4\xB9"},
{"larrsim;", "\xE2\xA5\xB3"},{"larrtl;", "\xE2\x86\xA2"},{"lat;", "\xE2\xAA\xAB"},{"latail;", "\xE2\xA4\x99"},
{"late;", "\xE2\xAA\xAD"},{"lates;", "\xE2\xAA\xAD\xEF\xB8\x80"},{"lbarr;", "\xE2\xA4\x8C"},{"lbbrk;", "\xE2\x9D\xB2"},
{"lbrace;", "\x7B"},{"lbrack;", "\x5B"},{"lbrke;", "\xE2\xA6\x8B"},{"lbrksld;", "\xE2\xA6\x8F"},
{"lbrkslu;", "\xE2\xA6\x8D"},{"lcaron;", "\xC4\xBE"},{"lcedil;", "\xC4\xBC"},{"lceil;", "\xE2\x8C\x88"},
{"lcub;", "\x7B"},{"lcy;", "\xD0\xBB"},{"ldca;", "\xE2\xA4\xB6"},{"ldquo;", "\xE2\x80\x9C"},
{"ldquor;", "\xE2\x80\x9E"},{"ldrdhar;", "\xE2\xA5\xA7"},{"ldrushar;", "\xE2\xA5\x8B"},{"ldsh;", "\xE2\x86\xB2"},
{"le;", "\xE2\x89\xA4"},{"leftarrow;", "\xE2\x86\x90"},{"leftarrowtail;", "\xE2\x86\xA2"},{"leftharpoondown;", "\xE2\x86\xBD"},
{"leftharpoonup;", "\xE2\x86\xBC"},{"leftleftarrows;", "\xE2\x87\x87"},{"leftrightarrow;", "\xE2\x86\x94"},{"leftrightarrows;", "\xE2\x87\x86"},
{"leftrightharpoons;", "\xE2\x87\x8B"},{"leftrightsquigarrow;", "\xE2\x86\xAD"},{"leftthreetimes;", "\xE2\x8B\x8B"},{"leg;", "\xE2\x8B\x9A"},
{"leq;", "\xE2\x89\xA4"},{"leqq;", "\xE2\x89\xA6"},{"leqslant;", "\xE2\xA9\xBD"},{"les;", "\xE2\xA9\xBD"},
{"lescc;", "\xE2\xAA\xA8"},{"lesdot;", "\xE2\xA9\xBF"},{"lesdoto;", "\xE2\xAA\x81"},{"lesdotor;", "\xE2\xAA\x83"},
{"lesg;", "\xE2\x8B\x9A\xEF\xB8\x80"},{"lesges;", "\xE2\xAA\x93"},{"lessapprox;", "\xE2\xAA\x85"},{"lessdot;", "\xE2\x8B\x96"},
{"lesseqgtr;", "\xE2\x8B\x9A"},{"lesseqqgtr;", "\xE2\xAA\x8B"},{"lessgtr;", "\xE2\x89\xB6"},{"lesssim;", "\xE2\x89\xB2"},
{"lfisht;", "\xE2\xA5\xBC"},{"lfloor;", "\xE2\x8C\x8A"},{"lfr;", "\xF0\x9D\x94\xA9"},{"lg;", "\xE2\x89\xB6"},
{"lgE;", "\xE2\xAA\x91"},{"lhard;", "\xE2\x86\xBD"},{"lharu;", "\xE2\x86\xBC"},{"lharul;", "\xE2\xA5\xAA"},
{"lhblk;", "\xE2\x96\x84"},{"ljcy;", "\xD1\x99"},{"ll;", "\xE2\x89\xAA"},{"llarr;", "\xE2\x87\x87"},
{"llcorner;", "\xE2\x8C\x9E"},{"llhard;", "\xE2\xA5\xAB"},{"lltri;", "\xE2\x97\xBA"},{"lmidot;", "\xC5\x80"},
{"lmoust;", "\xE2\x8E\xB0"},{"lmoustache;", "\xE2\x8E\xB0"},{"lnE;", "\xE2\x89\xA8"},{"lnap;", "\xE2\xAA\x89"},
{"lnapprox;", "\xE2\xAA\x89"},{"lne;", "\xE2\xAA\x87"},{"lneq;", "\xE2\xAA\x87"},{"lneqq;", "\xE2\x89\xA8"},
{"lnsim;", "\xE2\x8B\xA6"},{"loang;", "\xE2\x9F\xAC"},{"loarr;", "\xE2\x87\xBD"},{"lobrk;", "\xE2\x9F\xA6"},
{"longleftarrow;", "\xE2\x9F\xB5"},{"longleftrightarrow;", "\xE2\x9F\xB7"},{"longmapsto;", "\xE2\x9F\xBC"},{"longrightarrow;", "\xE2\x9F\xB6"},
{"looparrowleft;", "\xE2\x86\xAB"},{"looparrowright;", "\xE2\x86\xAC"},{"lopar;", "\xE2\xA6\x85"},{"lopf;", "\xF0\x9D\x95\x9D"},
{"loplus;", "\xE2\xA8\xAD"},{"lotimes;", "\xE2\xA8\xB4"},{"lowast;", "\xE2\x88\x97"},{"lowbar;", "\x5F"},
{"loz;", "\xE2\x97\x8A"},{"lozenge;", "\xE2\x97\x8A"},{"lozf;", "\xE2\xA7\xAB"},{"lpar;", "\x28"},
{"lparlt;", "\xE2\xA6\x93"},{"lrarr;", "\xE2\x87\x86"},{"lrcorner;", "\xE2\x8C\x9F"},{"lrhar;", "\xE2\x87\x8B"},
{"lrhard;", "\xE2\xA5\xAD"},{"lrm;", "\xE2\x80\x8E"},{"lrtri;", "\xE2\x8A\xBF"},{"lsaquo;", "\xE2\x80\xB9"},
{"lscr;", "\xF0\x9D\x93\x81"},{"lsh;", "\xE2\x86\xB0"},{"lsim;", "\xE2\x89\xB2"},{"lsime;", "\xE2\xAA\x8D"},
{"lsimg;", "\xE2\xAA\x8F"},{"lsqb;", "\x5B"},{"lsquo;", "\xE2\x80\x98"},{"lsquor;", "\xE2\x80\x9A"},
{"lstrok;", "\xC5\x82"},{"lt", "\x3C"},{"lt;", "\x3C"},{"ltcc;", "\xE2\xAA\xA6"},
{"ltcir;", "\xE2\xA9\xB9"},{"ltdot;", "\xE2\x8B\x96"},{"lthree;", "\xE2\x8B\x8B"},{"ltimes;", "\xE2\x8B\x89"},
{"ltlarr;", "\xE2\xA5\xB6"},{"ltquest;", "\xE2\xA9\xBB"},{"ltrPar;", "\xE2\xA6\x96"},{"ltri;", "\xE2\x97\x83"},
{"ltrie;", "\xE2\x8A\xB4"},{"ltrif;", "\xE2\x97\x82"},{"lurdshar;", "\xE2\xA5\x8A"},{"luruhar;", "\xE2\xA5\xA6"},
{"lvertneqq;", "\xE2\x89\xA8\xEF\xB8\x80"},{"lvnE;", "\xE2\x89\xA8\xEF\xB8\x80"},{"mDDot;", "\xE2\x88\xBA"},{"macr", "\xC2\xAF"},
{"macr;", "\xC2\xAF"},{"male;", "\xE2\x99\x82"},{"malt;", "\xE2\x9C\xA0"},{"maltese;", "\xE2\x9C\xA0"},
{"map;", "\xE2\x86\xA6"},{"mapsto;", "\xE2\x86\xA6"},{"mapstodown;", "\xE2\x86\xA7"},{"mapstoleft;", "\xE2\x86\xA4"},
{"mapstoup;", "\xE2\x86\xA5"},{"marker;", "\xE2\x96\xAE"},{"mcomma;", "\xE2\xA8\xA9"},{"mcy;", "\xD0\xBC"},
{"mdash;", "\xE2\x80\x94"},{"measuredangle;", "\xE2\x88\xA1"},{"mfr;", "\xF0\x9D\x94\xAA"},{"mho;", "\xE2\x84\xA7"},
{"micro", "\xC2\xB5"},{"micro;", "\xC2\xB5"},{"mid;", "\xE2\x88\xA3"},{"midast;", "\x2A"},
{"midcir;", "\xE2\xAB\xB0"},{"middot", "\xC2\xB7"},{"middot;", "\xC2\xB7"},{"minus;", "\xE2\x88\x92"},
{"minusb;", "\xE2\x8A\x9F"},{"minusd;", "\xE2\x88\xB8"},{"minusdu;", "\xE2\xA8\xAA"},{"mlcp;", "\xE2\xAB\x9B"},
{"mldr;", "\xE2\x80\xA6"},{"mnplus;", "\xE2\x88\x93"},{"models;", "\xE2\x8A\xA7"},{"mopf;", "\xF0\x9D\x95\x9E"},
{"mp;", "\xE2\x88\x93"},{"mscr;", "\xF0\x9D\x93\x82"},{"mstpos;", "\xE2\x88\xBE"},{"mu;", "\xCE\xBC"},
{"multimap;", "\xE2\x8A\xB8"},{"mumap;", "\xE2\x8A\xB8"},{"nGg;", "\xE2\x8B\x99\xCC\xB8"},{"nGt;", "\xE2\x89\xAB\xE2\x83\x92"},
{"nGtv;", "\xE2\x89\xAB\xCC\xB8"},{"nLeftarrow;", "\xE2\x87\x8D"},{"nLeftrightarrow;", "\xE2\x87\x8E"},{"nLl;", "\xE2\x8B\x98\xCC\xB8"},
{"nLt;", "\xE2\x89\xAA\xE2\x83\x92"},{"nLtv;", "\xE2\x89\xAA\xCC\xB8"},{"nRightarrow;", "\xE2\x87\x8F"},{"nVDash;", "\xE2\x8A\xAF"},
{"nVdash;", "\xE2\x8A\xAE"},{"nabla;", "\xE2\x88\x87"},{"nacute;", "\xC5\x84"},{"nang;", "\xE2\x88\xA0\xE2\x83\x92"},
{"nap;", "\xE2\x89\x89"},{"napE;", "\xE2\xA9\xB0\xCC\xB8"},{"napid;", "\xE2\x89\x8B\xCC\xB8"},{"napos;", "\xC5\x89"},
{"napprox;", "\xE2\x89\x89"},{"natur;", "\xE2\x99\xAE"},{"natural;", "\xE2\x99\xAE"},{"naturals;", "\xE2\x84\x95"},
{"nbsp", "\xC2\xA0"},{"nbsp;", "\xC2\xA0"},{"nbump;", "\xE2\x89\x8E\xCC\xB8"},{"nbumpe;", "\xE2\x89\x8F\xCC\xB8"},
{"ncap;", "\xE2\xA9\x83"},{"ncaron;", "\xC5\x88"},{"ncedil;", "\xC5\x86"},{"ncong;", "\xE2\x89\x87"},
{"ncongdot;", "\xE2\xA9\xAD\xCC\xB8"},{"ncup;", "\xE2\xA9\x82"},{"ncy;", "\xD0\xBD"},{"ndash;", "\xE2\x80\x93"},
{"ne;", "\xE2\x89\xA0"},{"neArr;", "\xE2\x87\x97"},{"nearhk;", "\xE2\xA4\xA4"},{"nearr;", "\xE2\x86\x97"},
{"nearrow;", "\xE2\x86\x97"},{"nedot;", "\xE2\x89\x90\xCC\xB8"},{"nequiv;", "\xE2\x89\xA2"},{"nesear;", "\xE2\xA4\xA8"},
{"nesim;", "\xE2\x89\x82\xCC\xB8"},{"nexist;", "\xE2\x88\x84"},{"nexists;", "\xE2\x88\x84"},{"nfr;", "\xF0\x9D\x94\xAB"},
{"ngE;", "\xE2\x89\xA7\xCC\xB8"},{"nge;", "\xE2\x89\xB1"},{"ngeq;", "\xE2\x89\xB1"},{"ngeqq;", "\xE2\x89\xA7\xCC\xB8"},
{"ngeqslant;", "\xE2\xA9\xBE\xCC\xB8"},{"nges;", "\xE2\xA9\xBE\xCC\xB8"},{"ngsim;", "\xE2\x89\xB5"},{"ngt;", "\xE2\x89\xAF"},
{"ngtr;", "\xE2\x89\xAF"},{"nhArr;", "\xE2\x87\x8E"},{"nharr;", "\xE2\x86\xAE"},{"nhpar;", "\xE2\xAB\xB2"},
{"ni;", "\xE2\x88\x8B"},{"nis;", "\xE2\x8B\xBC"},{"nisd;", "\xE2\x8B\xBA"},{"niv;", "\xE2\x88\x8B"},
{"njcy;", "\xD1\x9A"},{"nlArr;", "\xE2\x87\x8D"},{"nlE;", "\xE2\x89\xA6\xCC\xB8"},{"nlarr;", "\xE2\x86\x9A"},
{"nldr;", "\xE2\x80\xA5"},{"nle;", "\xE2\x89\xB0"},{"nleftarrow;", "\xE2\x86\x9A"},{"nleftrightarrow;", "\xE2\x86\xAE"},
{"nleq;", "\xE2\x89\xB0"},{"nleqq;", "\xE2\x89\xA6\xCC\xB8"},{"nleqslant;", "\xE2\xA9\xBD\xCC\xB8"},{"nles;", "\xE2\xA9\xBD\xCC\xB8"},
{"nless;", "\xE2\x89\xAE"},{"nlsim;", "\xE2\x89\xB4"},{"nlt;", "\xE2\x89\xAE"},{"nltri;", "\xE2\x8B\xAA"},
{"nltrie;", "\xE2\x8B\xAC"},{"nmid;", "\xE2\x88\xA4"},{"nopf;", "\xF0\x9D\x95\x9F"},{"not", "\xC2\xAC"},
{"not;", "\xC2\xAC"},{"notin;", "\xE2\x88\x89"},{"notinE;", "\xE2\x8B\xB9\xCC\xB8"},{"notindot;", "\xE2\x8B\xB5\xCC\xB8"},
{"notinva;", "\xE2\x88\x89"},{"notinvb;", "\xE2\x8B\xB7"},{"notinvc;", "\xE2\x8B\xB6"},{"notni;", "\xE2\x88\x8C"},
{"notniva;", "\xE2\x88\x8C"},{"notnivb;", "\xE2\x8B\xBE"},{"notnivc;", "\xE2\x8B\xBD"},{"npar;", "\xE2\x88\xA6"},
{"nparallel;", "\xE2\x88\xA6"},{"nparsl;", "\xE2\xAB\xBD\xE2\x83\xA5"},{"npart;", "\xE2\x88\x82\xCC\xB8"},{"npolint;", "\xE2\xA8\x94"},
{"npr;", "\xE2\x8A\x80"},{"nprcue;", "\xE2\x8B\xA0"},{"npre;", "\xE2\xAA\xAF\xCC\xB8"},{"nprec;", "\xE2\x8A\x80"},
{"npreceq;", "\xE2\xAA\xAF\xCC\xB8"},{"nrArr;", "\xE2\x87\x8F"},{"nrarr;", "\xE2\x86\x9B"},{"nrarrc;", "\xE2\xA4\xB3\xCC\xB8"},
{"nrarrw;", "\xE2\x86\x9D\xCC\xB8"},{"nrightarrow;", "\xE2\x86\x9B"},{"nrtri;", "\xE2\x8B\xAB"},{"nrtrie;", "\xE2\x8B\xAD"},
{"nsc;", "\xE2\x8A\x81"},{"nsccue;", "\xE2\x8B\xA1"},{"nsce;", "\xE2\xAA\xB0\xCC\xB8"},{"nscr;", "\xF0\x9D\x93\x83"},
{"nshortmid;", "\xE2\x88\xA4"},{"nshortparallel;", "\xE2\x88\xA6"},{"nsim;", "\xE2\x89\x81"},{"nsime;", "\xE2\x89\x84"},
{"nsimeq;", "\xE2\x89\x84"},{"nsmid;", "\xE2\x88\xA4"},{"nspar;", "\xE2\x88\xA6"},{"nsqsube;", "\xE2\x8B\xA2"},
{"nsqsupe;", "\xE2\x8B\xA3"},{"nsub;", "\xE2\x8A\x84"},{"nsubE;", "\xE2\xAB\x85\xCC\xB8"},{"nsube;", "\xE2\x8A\x88"},
{"nsubset;", "\xE2\x8A\x82\xE2\x83\x92"},{"nsubseteq;", "\xE2\x8A\x88"},{"nsubseteqq;", "\xE2\xAB\x85\xCC\xB8"},{"nsucc;", "\xE2\x8A\x81"},
{"nsucceq;", "\xE2\xAA\xB0\xCC\xB8"},{"nsup;", "\xE2\x8A\x85"},{"nsupE;", "\xE2\xAB\x86\xCC\xB8"},{"nsupe;", "\xE2\x8A\x89"},
{"nsupset;", "\xE2\x8A\x83\xE2\x83\x92"},{"nsupseteq;", "\xE2\x8A\x89"},{"nsupseteqq;", "\xE2\xAB\x86\xCC\xB8"},{"ntgl;", "\xE2\x89\xB9"},
{"ntilde", "\xC3\xB1"},{"ntilde;", "\xC3\xB1"},{"ntlg;", "\xE2\x89\xB8"},{"ntriangleleft;", "\xE2\x8B\xAA"},
{"ntrianglelefteq;", "\xE2\x8B\xAC"},{"ntriangleright;", "\xE2\x8B\xAB"},{"ntrianglerighteq;", "\xE2\x8B\xAD"},{"nu;", "\xCE\xBD"},
{"num;", "\x23"},{"numero;", "\xE2\x84\x96"},{"numsp;", "\xE2\x80\x87"},{"nvDash;", "\xE2\x8A\xAD"},
{"nvHarr;", "\xE2\xA4\x84"},{"nvap;", "\xE2\x89\x8D\xE2\x83\x92"},{"nvdash;", "\xE2\x8A\xAC"},{"nvge;", "\xE2\x89\xA5\xE2\x83\x92"},
{"nvgt;", "\x3E\xE2\x83\x92"},{"nvinfin;", "\xE2\xA7\x9E"},{"nvlArr;", "\xE2\xA4\x82"},{"nvle;", "\xE2\x89\xA4\xE2\x83\x92"},
{"nvlt;", "\x3C\xE2\x83\x92"},{"nvltrie;", "\xE2\x8A\xB4\xE2\x83\x92"},{"nvrArr;", "\xE2\xA4\x83"},{"nvrtrie;", "\xE2\x8A\xB5\xE2\x83\x92"},
{"nvsim;", "\xE2\x88\xBC\xE2\x83\x92"},{"nwArr;", "\xE2\x87\x96"},{"nwarhk;", "\xE2\xA4\xA3"},{"nwarr;", "\xE2\x86\x96"},
{"nwarrow;", "\xE2\x86\x96"},{"nwnear;", "\xE2\xA4\xA7"},{"oS;", "\xE2\x93\x88"},{"oacute", "\xC3\xB3"},
{"oacute;", "\xC3\xB3"},{"oast;", "\xE2\x8A\x9B"},{"ocir;", "\xE2\x8A\x9A"},{"ocirc", "\xC3\xB4"},
{"ocirc;", "\xC3\xB4"},{"ocy;", "\xD0\xBE"},{"odash;", "\xE2\x8A\x9D"},{"odblac;", "\xC5\x91"},
{"odiv;", "\xE2\xA8\xB8"},{"odot;", "\xE2\x8A\x99"},{"odsold;", "\xE2\xA6\xBC"},{"oelig;", "\xC5\x93"},
{"ofcir;", "\xE2\xA6\xBF"},{"ofr;", "\xF0\x9D\x94\xAC"},{"ogon;", "\xCB\x9B"},{"ograve", "\xC3\xB2"},
{"ograve;", "\xC3\xB2"},{"ogt;", "\xE2\xA7\x81"},{"ohbar;", "\xE2\xA6\xB5"},{"ohm;", "\xCE\xA9"},
{"oint;", "\xE2\x88\xAE"},{"olarr;", "\xE2\x86\xBA"},{"olcir;", "\xE2\xA6\xBE"},{"olcross;", "\xE2\xA6\xBB"},
{"oline;", "\xE2\x80\xBE"},{"olt;", "\xE2\xA7\x80"},{"omacr;", "\xC5\x8D"},{"omega;", "\xCF\x89"},
{"omicron;", "\xCE\xBF"},{"omid;", "\xE2\xA6\xB6"},{"ominus;", "\xE2\x8A\x96"},{"oopf;", "\xF0\x9D\x95\xA0"},
{"opar;", "\xE2\xA6\xB7"},{"operp;", "\xE2\xA6\xB9"},{"oplus;", "\xE2\x8A\x95"},{"or;", "\xE2\x88\xA8"},
{"orarr;", "\xE2\x86\xBB"},{"ord;", "\xE2\xA9\x9D"},{"order;", "\xE2\x84\xB4"},{"orderof;", "\xE2\x84\xB4"},
{"ordf", "\xC2\xAA"},{"ordf;", "\xC2\xAA"},{"ordm", "\xC2\xBA"},{"ordm;", "\xC2\xBA"},
{"origof;", "\xE2\x8A\xB6"},{"oror;", "\xE2\xA9\x96"},{"orslope;", "\xE2\xA9\x97"},{"orv;", "\xE2\xA9\x9B"},
{"oscr;", "\xE2\x84\xB4"},{"oslash", "\xC3\xB8"},{"oslash;", "\xC3\xB8"},{"osol;", "\xE2\x8A\x98"},
{"otilde", "\xC3\xB5"},{"otilde;", "\xC3\xB5"},{"otimes;", "\xE2\x8A\x97"},{"otimesas;", "\xE2\xA8\xB6"},
{"ouml", "\xC3\xB6"},{"ouml;", "\xC3\xB6"},{"ovbar;", "\xE2\x8C\xBD"},{"par;", "\xE2\x88\xA5"},
{"para", "\xC2\xB6"},{"para;", "\xC2\xB6"},{"parallel;", "\xE2\x88\xA5"},{"parsim;", "\xE2\xAB\xB3"},
{"parsl;", "\xE2\xAB\xBD"},{"part;", "\xE2\x88\x82"},{"pcy;", "\xD0\xBF"},{"percnt;", "\x25"},
{"period;", "\x2E"},{"permil;", "\xE2\x80\xB0"},{"perp;", "\xE2\x8A\xA5"},{"pertenk;", "\xE2\x80\xB1"},
{"pfr;", "\xF0\x9D\x94\xAD"},{"phi;", "\xCF\x86"},{"phiv;", "\xCF\x95"},{"phmmat;", "\xE2\x84\xB3"},
{"phone;", "\xE2\x98\x8E"},{"pi;", "\xCF\x80"},{"pitchfork;", "\xE2\x8B\x94"},{"piv;", "\xCF\x96"},
{"planck;", "\xE2\x84\x8F"},{"planckh;", "\xE2\x84\x8E"},{"plankv;", "\xE2\x84\x8F"},{"plus;", "\x2B"},
{"plusacir;", "\xE2\xA8\xA3"},{"plusb;", "\xE2\x8A\x9E"},{"pluscir;", "\xE2\xA8\xA2"},{"plusdo;", "\xE2\x88\x94"},
{"plusdu;", "\xE2\xA8\xA5"},{"pluse;", "\xE2\xA9\xB2"},{"plusmn", "\xC2\xB1"},{"plusmn;", "\xC2\xB1"},
{"plussim;", "\xE2\xA8\xA6"},{"plustwo;", "\xE2\xA8\xA7"},{"pm;", "\xC2\xB1"},{"pointint;", "\xE2\xA8\x95"},
{"popf;", "\xF0\x9D\x95\xA1"},{"pound", "\xC2\xA3"},{"pound;", "\xC2\xA3"},{"pr;", "\xE2\x89\xBA"},
{"prE;", "\xE2\xAA\xB3"},{"prap;", "\xE2\xAA\xB7"},{"prcue;", "\xE2\x89\xBC"},{"pre;", "\xE2\xAA\xAF"},
{"prec;", "\xE2\x89\xBA"},{"precapprox;", "\xE2\xAA\xB7"},{"preccurlyeq;", "\xE2\x89\xBC"},{"preceq;", "\xE2\xAA\xAF"},
{"precnapprox;", "\xE2\xAA\xB9"},{"precneqq;", "\xE2\xAA\xB5"},{"precnsim;", "\xE2\x8B\xA8"},{"precsim;", "\xE2\x89\xBE"},
{"prime;", "\xE2\x80\xB2"},{"primes;", "\xE2\x84\x99"},{"prnE;", "\xE2\xAA\xB5"},{"prnap;", "\xE2\xAA\xB9"},
{"prnsim;", "\xE2\x8B\xA8"},{"prod;", "\xE2\x88\x8F"},{"profalar;", "\xE2\x8C\xAE"},{"profline;", "\xE2\x8C\x92"},
{"profsurf;", "\xE2\x8C\x93"},{"prop;", "\xE2\x88\x9D"},{"propto;", "\xE2\x88\x9D"},{"prsim;", "\xE2\x89\xBE"},
{"prurel;", "\xE2\x8A\xB0"},{"pscr;", "\xF0\x9D\x93\x85"},{"psi;", "\xCF\x88"},{"puncsp;", "\xE2\x80\x88"},
{"qfr;", "\xF0\x9D\x94\xAE"},{"qint;", "\xE2\xA8\x8C"},{"qopf;", "\xF0\x9D\x95\xA2"},{"qprime;", "\xE2\x81\x97"},
{"qscr;", "\xF0\x9D\x93\x86"},{"quaternions;", "\xE2\x84\x8D"},{"quatint;", "\xE2\xA8\x96"},{"quest;", "\x3F"},
{"questeq;", "\xE2\x89\x9F"},{"quot", "\x22"},{"quot;", "\x22"},{"rAarr;", "\xE2\x87\x9B"},
{"rArr;", "\xE2\x87\x92"},{"rAtail;", "\xE2\xA4\x9C"},{"rBarr;", "\xE2\xA4\x8F"},{"rHar;", "\xE2\xA5\xA4"},
{"race;", "\xE2\x88\xBD\xCC\xB1"},{"racute;", "\xC5\x95"},{"radic;", "\xE2\x88\x9A"},{"raemptyv;", "\xE2\xA6\xB3"},
{"rang;", "\xE2\x9F\xA9"},{"rangd;", "\xE2\xA6\x92"},{"range;", "\xE2\xA6\xA5"},{"rangle;", "\xE2\x9F\xA9"},
{"raquo", "\xC2\xBB"},{"raquo;", "\xC2\xBB"},{"rarr;", "\xE2\x86\x92"},{"rarrap;", "\xE2\xA5\xB5"},
{"rarrb;", "\xE2\x87\xA5"},{"rarrbfs;", "\xE2\xA4\xA0"},{"rarrc;", "\xE2\xA4\xB3"},{"rarrfs;", "\xE2\xA4\x9E"},
{"rarrhk;", "\xE2\x86\xAA"},{"rarrlp;", "\xE2\x86\xAC"},{"rarrpl;", "\xE2\xA5\x85"},{"rarrsim;", "\xE2\xA5\xB4"},
{"rarrtl;", "\xE2\x86\xA3"},{"rarrw;", "\xE2\x86\x9D"},{"ratail;", "\xE2\xA4\x9A"},{"ratio;", "\xE2\x88\xB6"},
{"rationals;", "\xE2\x84\x9A"},{"rbarr;", "\xE2\xA4\x8D"},{"rbbrk;", "\xE2\x9D\xB3"},{"rbrace;", "\x7D"},
{"rbrack;", "\x5D"},{"rbrke;", "\xE2\xA6\x8C"},{"rbrksld;", "\xE2\xA6\x8E"},{"rbrkslu;", "\xE2\xA6\x90"},
{"rcaron;", "\xC5\x99"},{"rcedil;", "\xC5\x97"},{"rceil;", "\xE2\x8C\x89"},{"rcub;", "\x7D"},
{"rcy;", "\xD1\x80"},{"rdca;", "\xE2\xA4\xB7"},{"rdldhar;", "\xE2\xA5\xA9"},{"rdquo;", "\xE2\x80\x9D"},
{"rdquor;", "\xE2\x80\x9D"},{"rdsh;", "\xE2\x86\xB3"},{"real;", "\xE2\x84\x9C"},{"realine;", "\xE2\x84\x9B"},
{"realpart;", "\xE2\x84\x9C"},{"reals;", "\xE2\x84\x9D"},{"rect;", "\xE2\x96\xAD"},{"reg", "\xC2\xAE"},
{"reg;", "\xC2\xAE"},{"rfisht;", "\xE2\xA5\xBD"},{"rfloor;", "\xE2\x8C\x8B"},{"rfr;", "\xF0\x9D\x94\xAF"},
{"rhard;", "\xE2\x87\x81"},{"rharu;", "\xE2\x87\x80"},{"rharul;", "\xE2\xA5\xAC"},{"rho;", "\xCF\x81"},
{"rhov;", "\xCF\xB1"},{"rightarrow;", "\xE2\x86\x92"},{"rightarrowtail;", "\xE2\x86\xA3"},{"rightharpoondown;", "\xE2\x87\x81"},
{"rightharpoonup;", "\xE2\x87\x80"},{"rightleftarrows;", "\xE2\x87\x84"},{"rightleftharpoons;", "\xE2\x87\x8C"},{"rightrightarrows;", "\xE2\x87\x89"},
{"rightsquigarrow;", "\xE2\x86\x9D"},{"rightthreetimes;", "\xE2\x8B\x8C"},{"ring;", "\xCB\x9A"},{"risingdotseq;", "\xE2\x89\x93"},
{"rlarr;", "\xE2\x87\x84"},{"rlhar;", "\xE2\x87\x8C"},{"rlm;", "\xE2\x80\x8F"},{"rmoust;", "\xE2\x8E\xB1"},
{"rmoustache;", "\xE2\x8E\xB1"},{"rnmid;", "\xE2\xAB\xAE"},{"roang;", "\xE2\x9F\xAD"},{"roarr;", "\xE2\x87\xBE"},
{"robrk;", "\xE2\x9F\xA7"},{"ropar;", "\xE2\xA6\x86"},{"ropf;", "\xF0\x9D\x95\xA3"},{"roplus;", "\xE2\xA8\xAE"},
{"rotimes;", "\xE2\xA8\xB5"},{"rpar;", "\x29"},{"rpargt;", "\xE2\xA6\x94"},{"rppolint;", "\xE2\xA8\x92"},
{"rrarr;", "\xE2\x87\x89"},{"rsaquo;", "\xE2\x80\xBA"},{"rscr;", "\xF0\x9D\x93\x87"},{"rsh;", "\xE2\x86\xB1"},
{"rsqb;", "\x5D"},{"rsquo;", "\xE2\x80\x99"},{"rsquor;", "\xE2\x80\x99"},{"rthree;", "\xE2\x8B\x8C"},
{"rtimes;", "\xE2\x8B\x8A"},{"rtri;", "\xE2\x96\xB9"},{"rtrie;", "\xE2\x8A\xB5"},{"rtrif;", "\xE2\x96\xB8"},
{"rtriltri;", "\xE2\xA7\x8E"},{"ruluhar;", "\xE2\xA5\xA8"},{"rx;", "\xE2\x84\x9E"},{"sacute;", "\xC5\x9B"},
{"sbquo;", "\xE2\x80\x9A"},{"sc;", "\xE2\x89\xBB"},{"scE;", "\xE2\xAA\xB4"},{"scap;", "\xE2\xAA\xB8"},
{"scaron;", "\xC5\xA1"},{"sccue;", "\xE2\x89\xBD"},{"sce;", "\xE2\xAA\xB0"},{"scedil;", "\xC5\x9F"},
{"scirc;", "\xC5\x9D"},{"scnE;", "\xE2\xAA\xB6"},{"scnap;", "\xE2\xAA\xBA"},{"scnsim;", "\xE2\x8B\xA9"},
{"scpolint;", "\xE2\xA8\x93"},{"scsim;", "\xE2\x89\xBF"},{"scy;", "\xD1\x81"},{"sdot;", "\xE2\x8B\x85"},
{"sdotb;", "\xE2\x8A\xA1"},{"sdote;", "\xE2\xA9\xA6"},{"seArr;", "\xE2\x87\x98"},{"searhk;", "\xE2\xA4\xA5"},
{"searr;", "\xE2\x86\x98"},{"searrow;", "\xE2\x86\x98"},{"sect", "\xC2\xA7"},{"sect;", "\xC2\xA7"},
{"semi;", "\x3B"},{"seswar;", "\xE2\xA4\xA9"},{"setminus;", "\xE2\x88\x96"},{"setmn;", "\xE2\x88\x96"},
{"sext;", "\xE2\x9C\xB6"},{"sfr;", "\xF0\x9D\x94\xB0"},{"sfrown;", "\xE2\x8C\xA2"},{"sharp;", "\xE2\x99\xAF"},
{"shchcy;", "\xD1\x89"},{"shcy;", "\xD1\x88"},{"shortmid;", "\xE2\x88\xA3"},{"shortparallel;", "\xE2\x88\xA5"},
{"shy", "\xC2\xAD"},{"shy;", "\xC2\xAD"},{"sigma;", "\xCF\x83"},{"sigmaf;", "\xCF\x82"},
{"sigmav;", "\xCF\x82"},{"sim;", "\xE2\x88\xBC"},{"simdot;", "\xE2\xA9\xAA"},{"sime;", "\xE2\x89\x83"},
{"simeq;", "\xE2\x89\x83"},{"simg;", "\xE2\xAA\x9E"},{"simgE;", "\xE2\xAA\xA0"},{"siml;", "\xE2\xAA\x9D"},
{"simlE;", "\xE2\xAA\x9F"},{"simne;", "\xE2\x89\x86"},{"simplus;", "\xE2\xA8\xA4"},{"simrarr;", "\xE2\xA5\xB2"},
{"slarr;", "\xE2\x86\x90"},{"smallsetminus;", "\xE2\x88\x96"},{"smashp;", "\xE2\xA8\xB3"},{"smeparsl;", "\xE2\xA7\xA4"},
{"smid;", "\xE2\x88\xA3"},{"smile;", "\xE2\x8C\xA3"},{"smt;", "\xE2\xAA\xAA"},{"smte;", "\xE2\xAA\xAC"},
{"smtes;", "\xE2\xAA\xAC\xEF\xB8\x80"},{"softcy;", "\xD1\x8C"},{"sol;", "\x2F"},{"solb;", "\xE2\xA7\x84"},
{"solbar;", "\xE2\x8C\xBF"},{"sopf;", "\xF0\x9D\x95\xA4"},{"spades;", "\xE2\x99\xA0"},{"spadesuit;", "\xE2\x99\xA0"},
{"spar;", "\xE2\x88\xA5"},{"sqcap;", "\xE2\x8A\x93"},{"sqcaps;", "\xE2\x8A\x93\xEF\xB8\x80"},{"sqcup;", "\xE2\x8A\x94"},
{"sqcups;", "\xE2\x8A\x94\xEF\xB8\x80"},{"sqsub;", "\xE2\x8A\x8F"},{"sqsube;", "\xE2\x8A\x91"},{"sqsubset;", "\xE2\x8A\x8F"},
{"sqsubseteq;", "\xE2\x8A\x91"},{"sqsup;", "\xE2\x8A\x90"},{"sqsupe;", "\xE2\x8A\x92"},{"sqsupset;", "\xE2\x8A\x90"},
{"sqsupseteq;", "\xE2\x8A\x92"},{"squ;", "\xE2\x96\xA1"},{"square;", "\xE2\x96\xA1"},{"squarf;", "\xE2\x96\xAA"},
{"squf;", "\xE2\x96\xAA"},{"srarr;", "\xE2\x86\x92"},{"sscr;", "\xF0\x9D\x93\x88"},{"ssetmn;", "\xE2\x88\x96"},
{"ssmile;", "\xE2\x8C\xA3"},{"sstarf;", "\xE2\x8B\x86"},{"star;", "\xE2\x98\x86"},{"starf;", "\xE2\x98\x85"},
{"straightepsilon;", "\xCF\xB5"},{"straightphi;", "\xCF\x95"},{"strns;", "\xC2\xAF"},{"sub;", "\xE2\x8A\x82"},
{"subE;", "\xE2\xAB\x85"},{"subdot;", "\xE2\xAA\xBD"},{"sube;", "\xE2\x8A\x86"},{"subedot;", "\xE2\xAB\x83"},
{"submult;", "\xE2\xAB\x81"},{"subnE;", "\xE2\xAB\x8B"},{"subne;", "\xE2\x8A\x8A"},{"subplus;", "\xE2\xAA\xBF"},
{"subrarr;", "\xE2\xA5\xB9"},{"subset;", "\xE2\x8A\x82"},{"subseteq;", "\xE2\x8A\x86"},{"subseteqq;", "\xE2\xAB\x85"},
{"subsetneq;", "\xE2\x8A\x8A"},{"subsetneqq;", "\xE2\xAB\x8B"},{"subsim;", "\xE2\xAB\x87"},{"subsub;", "\xE2\xAB\x95"},
{"subsup;", "\xE2\xAB\x93"},{"succ;", "\xE2\x89\xBB"},{"succapprox;", "\xE2\xAA\xB8"},{"succcurlyeq;", "\xE2\x89\xBD"},
{"succeq;", "\xE2\xAA\xB0"},{"succnapprox;", "\xE2\xAA\xBA"},{"succneqq;", "\xE2\xAA\xB6"},{"succnsim;", "\xE2\x8B\xA9"},
{"succsim;", "\xE2\x89\xBF"},{"sum;", "\xE2\x88\x91"},{"sung;", "\xE2\x99\xAA"},{"sup1", "\xC2\xB9"},
{"sup1;", "\xC2\xB9"},{"sup2", "\xC2\xB2"},{"sup2;", "\xC2\xB2"},{"sup3", "\xC2\xB3"},
{"sup3;", "\xC2\xB3"},{"sup;", "\xE2\x8A\x83"},{"supE;", "\xE2\xAB\x86"},{"supdot;", "\xE2\xAA\xBE"},
{"supdsub;", "\xE2\xAB\x98"},{"supe;", "\xE2\x8A\x87"},{"supedot;", "\xE2\xAB\x84"},{"suphsol;", "\xE2\x9F\x89"},
{"suphsub;", "\xE2\xAB\x97"},{"suplarr;", "\xE2\xA5\xBB"},{"supmult;", "\xE2\xAB\x82"},{"supnE;", "\xE2\xAB\x8C"},
{"supne;", "\xE2\x8A\x8B"},{"supplus;", "\xE2\xAB\x80"},{"supset;", "\xE2\x8A\x83"},{"supseteq;", "\xE2\x8A\x87"},
{"supseteqq;", "\xE2\xAB\x86"},{"supsetneq;", "\xE2\x8A\x8B"},{"supsetneqq;", "\xE2\xAB\x8C"},{"supsim;", "\xE2\xAB\x88"},
{"supsub;", "\xE2\xAB\x94"},{"supsup;", "\xE2\xAB\x96"},{"swArr;", "\xE2\x87\x99"},{"swarhk;", "\xE2\xA4\xA6"},
{"swarr;", "\xE2\x86\x99"},{"swarrow;", "\xE2\x86\x99"},{"swnwar;", "\xE2\xA4\xAA"},{"szlig", "\xC3\x9F"},
{"szlig;", "\xC3\x9F"},{"target;", "\xE2\x8C\x96"},{"tau;", "\xCF\x84"},{"tbrk;", "\xE2\x8E\xB4"},
{"tcaron;", "\xC5\xA5"},{"tcedil;", "\xC5\xA3"},{"tcy;", "\xD1\x82"},{"tdot;", "\xE2\x83\x9B"},
{"telrec;", "\xE2\x8C\x95"},{"tfr;", "\xF0\x9D\x94\xB1"},{"there4;", "\xE2\x88\xB4"},{"therefore;", "\xE2\x88\xB4"},
{"theta;", "\xCE\xB8"},{"thetasym;", "\xCF\x91"},{"thetav;", "\xCF\x91"},{"thickapprox;", "\xE2\x89\x88"},
{"thicksim;", "\xE2\x88\xBC"},{"thinsp;", "\xE2\x80\x89"},{"thkap;", "\xE2\x89\x88"},{"thksim;", "\xE2\x88\xBC"},
{"thorn", "\xC3\xBE"},{"thorn;", "\xC3\xBE"},{"tilde;", "\xCB\x9C"},{"times", "\xC3\x97"},
{"times;", "\xC3\x97"},{"timesb;", "\xE2\x8A\xA0"},{"timesbar;", "\xE2\xA8\xB1"},{"timesd;", "\xE2\xA8\xB0"},
{"tint;", "\xE2\x88\xAD"},{"toea;", "\xE2\xA4\xA8"},{"top;", "\xE2\x8A\xA4"},{"topbot;", "\xE2\x8C\xB6"},
{"topcir;", "\xE2\xAB\xB1"},{"topf;", "\xF0\x9D\x95\xA5"},{"topfork;", "\xE2\xAB\x9A"},{"tosa;", "\xE2\xA4\xA9"},
{"tprime;", "\xE2\x80\xB4"},{"trade;", "\xE2\x84\xA2"},{"triangle;", "\xE2\x96\xB5"},{"triangledown;", "\xE2\x96\xBF"},
{"triangleleft;", "\xE2\x97\x83"},{"trianglelefteq;", "\xE2\x8A\xB4"},{"triangleq;", "\xE2\x89\x9C"},{"triangleright;", "\xE2\x96\xB9"},
{"trianglerighteq;", "\xE2\x8A\xB5"},{"tridot;", "\xE2\x97\xAC"},{"trie;", "\xE2\x89\x9C"},{"triminus;", "\xE2\xA8\xBA"},
{"triplus;", "\xE2\xA8\xB9"},{"trisb;", "\xE2\xA7\x8D"},{"tritime;", "\xE2\xA8\xBB"},{"trpezium;", "\xE2\x8F\xA2"},
{"tscr;", "\xF0\x9D\x93\x89"},{"tscy;", "\xD1\x86"},{"tshcy;", "\xD1\x9B"},{"tstrok;", "\xC5\xA7"},
{"twixt;", "\xE2\x89\xAC"},{"twoheadleftarrow;", "\xE2\x86\x9E"},{"twoheadrightarrow;", "\xE2\x86\xA0"},{"uArr;", "\xE2\x87\x91"},
{"uHar;", "\xE2\xA5\xA3"},{"uacute", "\xC3\xBA"},{"uacute;", "\xC3\xBA"},{"uarr;", "\xE2\x86\x91"},
{"ubrcy;", "\xD1\x9E"},{"ubreve;", "\xC5\xAD"},{"ucirc", "\xC3\xBB"},{"ucirc;", "\xC3\xBB"},
{"ucy;", "\xD1\x83"},{"udarr;", "\xE2\x87\x85"},{"udblac;", "\xC5\xB1"},{"udhar;", "\xE2\xA5\xAE"},
{"ufisht;", "\xE2\xA5\xBE"},{"ufr;", "\xF0\x9D\x94\xB2"},{"ugrave", "\xC3\xB9"},{"ugrave;", "\xC3\xB9"},
{"uharl;", "\xE2\x86\xBF"},{"uharr;", "\xE2\x86\xBE"},{"uhblk;", "\xE2\x96\x80"},{"ulcorn;", "\xE2\x8C\x9C"},
{"ulcorner;", "\xE2\x8C\x9C"},{"ulcrop;", "\xE2\x8C\x8F"},{"ultri;", "\xE2\x97\xB8"},{"umacr;", "\xC5\xAB"},
{"uml", "\xC2\xA8"},{"uml;", "\xC2\xA8"},{"uogon;", "\xC5\xB3"},{"uopf;", "\xF0\x9D\x95\xA6"},
{"uparrow;", "\xE2\x86\x91"},{"updownarrow;", "\xE2\x86\x95"},{"upharpoonleft;", "\xE2\x86\xBF"},{"upharpoonright;", "\xE2\x86\xBE"},
{"uplus;", "\xE2\x8A\x8E"},{"upsi;", "\xCF\x85"},{"upsih;", "\xCF\x92"},{"upsilon;", "\xCF\x85"},
{"upuparrows;", "\xE2\x87\x88"},{"urcorn;", "\xE2\x8C\x9D"},{"urcorner;", "\xE2\x8C\x9D"},{"urcrop;", "\xE2\x8C\x8E"},
{"uring;", "\xC5\xAF"},{"urtri;", "\xE2\x97\xB9"},{"uscr;", "\xF0\x9D\x93\x8A"},{"utdot;", "\xE2\x8B\xB0"},
{"utilde;", "\xC5\xA9"},{"utri;", "\xE2\x96\xB5"},{"utrif;", "\xE2\x96\xB4"},{"uuarr;", "\xE2\x87\x88"},
{"uuml", "\xC3\xBC"},{"uuml;", "\xC3\xBC"},{"uwangle;", "\xE2\xA6\xA7"},{"vArr;", "\xE2\x87\x95"},
{"vBar;", "\xE2\xAB\xA8"},{"vBarv;", "\xE2\xAB\xA9"},{"vDash;", "\xE2\x8A\xA8"},{"vangrt;", "\xE2\xA6\x9C"},
{"varepsilon;", "\xCF\xB5"},{"varkappa;", "\xCF\xB0"},{"varnothing;", "\xE2\x88\x85"},{"varphi;", "\xCF\x95"},
{"varpi;", "\xCF\x96"},{"varpropto;", "\xE2\x88\x9D"},{"varr;", "\xE2\x86\x95"},{"varrho;", "\xCF\xB1"},
{"varsigma;", "\xCF\x82"},{"varsubsetneq;", "\xE2\x8A\x8A\xEF\xB8\x80"},{"varsubsetneqq;", "\xE2\xAB\x8B\xEF\xB8\x80"},{"varsupsetneq;", "\xE2\x8A\x8B\xEF\xB8\x80"},
{"varsupsetneqq;", "\xE2\xAB\x8C\xEF\xB8\x80"},{"vartheta;", "\xCF\x91"},{"vartriangleleft;", "\xE2\x8A\xB2"},{"vartriangleright;", "\xE2\x8A\xB3"},
{"vcy;", "\xD0\xB2"},{"vdash;", "\xE2\x8A\xA2"},{"vee;", "\xE2\x88\xA8"},{"veebar;", "\xE2\x8A\xBB"},
{"veeeq;", "\xE2\x89\x9A"},{"vellip;", "\xE2\x8B\xAE"},{"verbar;", "\x7C"},{"vert;", "\x7C"},
{"vfr;", "\xF0\x9D\x94\xB3"},{"vltri;", "\xE2\x8A\xB2"},{"vnsub;", "\xE2\x8A\x82\xE2\x83\x92"},{"vnsup;", "\xE2\x8A\x83\xE2\x83\x92"},
{"vopf;", "\xF0\x9D\x95\xA7"},{"vprop;", "\xE2\x88\x9D"},{"vrtri;", "\xE2\x8A\xB3"},{"vscr;", "\xF0\x9D\x93\x8B"},
{"vsubnE;", "\xE2\xAB\x8B\xEF\xB8\x80"},{"vsubne;", "\xE2\x8A\x8A\xEF\xB8\x80"},{"vsupnE;", "\xE2\xAB\x8C\xEF\xB8\x80"},{"vsupne;", "\xE2\x8A\x8B\xEF\xB8\x80"},
{"vzigzag;", "\xE2\xA6\x9A"},{"wcirc;", "\xC5\xB5"},{"wedbar;", "\xE2\xA9\x9F"},{"wedge;", "\xE2\x88\xA7"},
{"wedgeq;", "\xE2\x89\x99"},{"weierp;", "\xE2\x84\x98"},{"wfr;", "\xF0\x9D\x94\xB4"},{"wopf;", "\xF0\x9D\x95\xA8"},
{"wp;", "\xE2\x84\x98"},{"wr;", "\xE2\x89\x80"},{"wreath;", "\xE2\x89\x80"},{"wscr;", "\xF0\x9D\x93\x8C"},
{"xcap;", "\xE2\x8B\x82"},{"xcirc;", "\xE2\x97\xAF"},{"xcup;", "\xE2\x8B\x83"},{"xdtri;", "\xE2\x96\xBD"},
{"xfr;", "\xF0\x9D\x94\xB5"},{"xhArr;", "\xE2\x9F\xBA"},{"xharr;", "\xE2\x9F\xB7"},{"xi;", "\xCE\xBE"},
{"xlArr;", "\xE2\x9F\xB8"},{"xlarr;", "\xE2\x9F\xB5"},{"xmap;", "\xE2\x9F\xBC"},{"xnis;", "\xE2\x8B\xBB"},
{"xodot;", "\xE2\xA8\x80"},{"xopf;", "\xF0\x9D\x95\xA9"},{"xoplus;", "\xE2\xA8\x81"},{"xotime;", "\xE2\xA8\x82"},
{"xrArr;", "\xE2\x9F\xB9"},{"xrarr;", "\xE2\x9F\xB6"},{"xscr;", "\xF0\x9D\x93\x8D"},{"xsqcup;", "\xE2\xA8\x86"},
{"xuplus;", "\xE2\xA8\x84"},{"xutri;", "\xE2\x96\xB3"},{"xvee;", "\xE2\x8B\x81"},{"xwedge;", "\xE2\x8B\x80"},
{"yacute", "\xC3\xBD"},{"yacute;", "\xC3\xBD"},{"yacy;", "\xD1\x8F"},{"ycirc;", "\xC5\xB7"},
{"ycy;", "\xD1\x8B"},{"yen", "\xC2\xA5"},{"yen;", "\xC2\xA5"},{"yfr;", "\xF0\x9D\x94\xB6"},
{"yicy;", "\xD1\x97"},{"yopf;", "\xF0\x9D\x95\xAA"},{"yscr;", "\xF0\x9D\x93\x8E"},{"yucy;", "\xD1\x8E"},
{"yuml", "\xC3\xBF"},{"yuml;", "\xC3\xBF"},{"zacute;", "\xC5\xBA"},{"zcaron;", "\xC5\xBE"},
{"zcy;", "\xD0\xB7"},{"zdot;", "\xC5\xBC"},{"zeetrf;", "\xE2\x84\xA8"},{"zeta;", "\xCE\xB6"},
{"zfr;", "\xF0\x9D\x94\xB7"},{"zhcy;", "\xD0\xB6"},{"zigrarr;", "\xE2\x87\x9D"},{"zopf;", "\xF0\x9D\x95\xAB"},
{"zscr;", "\xF0\x9D\x93\x8F"},{"zwj;", "\xE2\x80\x8D"},{"zwnj;", "\xE2\x80\x8C"},
};

bool lookup_entity(std::string_view name, std::string& value) {
    const HtmlEntity* begin = kHtml5Entities;
    const HtmlEntity* end =
        kHtml5Entities + sizeof(kHtml5Entities) / sizeof(kHtml5Entities[0]);
    const HtmlEntity* it =
        std::lower_bound(begin, end, name, [](const HtmlEntity& e, std::string_view n) {
            return std::string_view(e.name) < n;
        });
    if (it == end || std::string_view(it->name) != name) return false;
    value = it->value;
    return true;
}

void append_utf8_cp(unsigned long long cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

// Python html._replace_charref 的数字引用分支（_invalid_charrefs → 代理区/越界
// → _invalid_codepoints 删除 → 正常码点）
void append_numeric_ref(unsigned long long cp, std::string& out) {
    if (cp == 0x00) { out += "\xEF\xBF\xBD"; return; }  // U+FFFD
    if (cp == 0x0D) { out.push_back('\r'); return; }
    if (cp >= 0x80 && cp <= 0x9F) {
        // 0x80..0x9F 按 Windows-1252 重映射（个别控制符保持原值），直接输出
        static const unsigned kCp1252[32] = {
            0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
            0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
        };
        append_utf8_cp(kCp1252[cp - 0x80], out);
        return;
    }
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        out += "\xEF\xBF\xBD";
        return;
    }
    if ((cp >= 0x01 && cp <= 0x08) || cp == 0x0B || (cp >= 0x0E && cp <= 0x1F) ||
        (cp >= 0x7F && cp <= 0x9F) || (cp >= 0xFDD0 && cp <= 0xFDEF) ||
        ((cp & 0xFFFF) >= 0xFFFE)) {
        return;  // Python _invalid_codepoints：删除（不产出字符）
    }
    append_utf8_cp(cp, out);
}

std::string decode_entities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        if (s[i] != '&') { out.push_back(s[i++]); continue; }
        const size_t p = i + 1;
        // 数字引用：&#123; / &#x1F600;（分号可省，与 html._charref 的正则一致）
        if (p < n && s[p] == '#') {
            size_t q = p + 1;
            bool hex = false;
            if (q < n && (s[q] == 'x' || s[q] == 'X')) {
                hex = true;
                ++q;
            }
            const size_t first = q;
            unsigned long long v = 0;
            while (q < n) {
                const char c = s[q];
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                if (d < 0) break;
                // Python 是任意精度整数；这里只关心"是否越界"，封顶防溢出
                if (v < 0x110000ull) v = v * (hex ? 16ull : 10ull) + (unsigned long long)d;
                if (v > 0x110000ull) v = 0x110000ull;
                ++q;
            }
            if (q > first) {
                if (q < n && s[q] == ';') ++q;
                append_numeric_ref(v, out);
                i = q;
                continue;
            }
            out.push_back('&');  // '&#' 后无数字：正则不匹配，按字面量处理
            ++i;
            continue;
        }
        // 命名引用：body ≤32 且不含 \t \n \f 空格 < & # ;，可选 ';' 收尾
        size_t b = p;
        while (b < n && b - p < 32) {
            const char c = s[b];
            if (c == '\t' || c == '\n' || c == '\f' || c == ' ' || c == '<' || c == '&' ||
                c == '#' || c == ';')
                break;
            ++b;
        }
        if (b == p) {
            out.push_back('&');
            ++i;
            continue;
        }
        const bool semi = b < n && s[b] == ';';
        const std::string_view token(s.data() + p, b - p + (semi ? 1 : 0));
        std::string value;
        if (lookup_entity(token, value)) {
            out += value;
        } else {
            bool matched = false;
            // Python：找最长可匹配前缀（长度 len-1..2），余下部分按字面量拼接
            for (size_t x = token.size() - 1; x >= 2; --x) {
                if (lookup_entity(token.substr(0, x), value)) {
                    out += value;
                    out.append(token.data() + x, token.size() - x);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                out.push_back('&');
                out.append(token.data(), token.size());
            }
        }
        i = p + token.size();
    }
    return out;
}

// ============================================================ 轻量 HTML 树
// children 用 deque：解析期间入栈的元素指针必须稳定（vector 会整块搬移）
struct HNode {
    bool is_text = false;
    std::string name;                                  // 元素名（小写）
    std::map<std::string, std::string> attrs;          // 属性（键小写，值已实体解码）
    std::string text;                                  // 文本节点内容（已实体解码）
    std::list<HNode> children;  // std::list 允许不完整类型

    HNode* add_element(std::string n) {
        children.emplace_back();
        children.back().name = std::move(n);
        return &children.back();
    }
    void add_text(std::string t) {
        if (t.empty()) return;
        children.emplace_back();
        children.back().is_text = true;
        children.back().text = std::move(t);
    }
};

inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// 手写容错 HTML 解析：标签流 + 实体；script/style 原始内容整体跳过
//
// 深度上限：树本身必须有限深 —— 后续所有遍历（has_block_descendant / epub_walk /
// find_body / collect_spans / gather_all_text）以及 std::list<HNode> 的递归析构都按层
// 递归，实测 6 万层 <div> 会让整个进程 STATUS_STACK_OVERFLOW 死亡（审查 B3）。
// 超过上限的**整棵子树被丢弃**（不是拍平成兄弟节点）：Python 对这类文档给出
// 0 字符 / 0 页，丢弃子树才能与之一致；正常文档（深度 << 200）完全不受影响。
constexpr size_t kMaxHtmlDepth = 200;

HNode parse_html_tree(const std::string& src) {
    HNode root;
    root.name = "#root";
    std::vector<HNode*> stack{&root};
    // >0 表示正在丢弃一个"超过深度上限"的子树：其中的文本与标签全部忽略，
    // 只数开/闭标签以找到该子树的结尾。
    size_t skipped_depth = 0;
    static const char* const kVoidTags[] = {"area", "base", "br", "col", "embed", "hr",
                                            "img", "input", "link", "meta", "param",
                                            "source", "track", "wbr"};
    auto is_void = [](const std::string& n) {
        for (const char* t : kVoidTags)
            if (n == t) return true;
        return false;
    };

    size_t i = 0, n = src.size();
    while (i < n) {
        size_t lt = src.find('<', i);
        if (lt == std::string::npos) {
            if (skipped_depth == 0) stack.back()->add_text(decode_entities(src.substr(i)));
            break;
        }
        if (lt > i && skipped_depth == 0)
            stack.back()->add_text(decode_entities(src.substr(i, lt - i)));
        i = lt;
        if (i + 1 >= n) break;
        // 注释
        if (src.compare(i, 4, "<!--") == 0) {
            size_t end = src.find("-->", i + 4);
            i = end == std::string::npos ? n : end + 3;
            continue;
        }
        // DOCTYPE / 处理指令
        if (src[i + 1] == '!' || src[i + 1] == '?') {
            size_t end = src.find('>', i);
            i = end == std::string::npos ? n : end + 1;
            continue;
        }
        // 结束标签
        if (src[i + 1] == '/') {
            size_t end = src.find('>', i);
            if (skipped_depth > 0) {   // 被丢弃的子树：只数层级，不弹栈
                skipped_depth--;
                i = end == std::string::npos ? n : end + 1;
                continue;
            }
            std::string tag = to_lower(src.substr(i + 2, (end == std::string::npos ? n : end) - i - 2));
            // 去掉尾部空白
            while (!tag.empty() && is_space(tag.back())) tag.pop_back();
            // 弹栈直到弹出同名元素（找不到就整条忽略，容错）
            if (!tag.empty()) {
                for (size_t k = stack.size(); k-- > 1;) {
                    if (stack[k]->name == tag) {
                        stack.resize(k);
                        break;
                    }
                }
            }
            i = end == std::string::npos ? n : end + 1;
            continue;
        }
        // 开始标签
        size_t j = i + 1;
        while (j < n && !is_space(src[j]) && src[j] != '>' && src[j] != '/') ++j;
        std::string tag = to_lower(src.substr(i + 1, j - i - 1));
        if (tag.empty()) {  // "<" 后不是标签，当普通文本
            if (skipped_depth == 0) stack.back()->add_text("<");
            i = i + 1;
            continue;
        }
        bool too_deep = skipped_depth > 0 || stack.size() > kMaxHtmlDepth;
        HNode* node = too_deep ? nullptr : stack.back()->add_element(tag);
        bool self_closed = false;
        // 属性解析
        while (j < n) {
            while (j < n && is_space(src[j])) ++j;
            if (j >= n) break;
            if (src[j] == '>') { ++j; break; }
            if (src[j] == '/') {  // 自闭合
                if (j + 1 < n && src[j + 1] == '>') { self_closed = true; j += 2; break; }
                ++j;
                continue;
            }
            size_t ks = j;
            while (j < n && !is_space(src[j]) && src[j] != '=' && src[j] != '>' && src[j] != '/') ++j;
            std::string key = to_lower(src.substr(ks, j - ks));
            while (j < n && is_space(src[j])) ++j;
            std::string value;
            if (j < n && src[j] == '=') {
                ++j;
                while (j < n && is_space(src[j])) ++j;
                if (j < n && (src[j] == '"' || src[j] == '\'')) {
                    char q = src[j++];
                    size_t ve = src.find(q, j);
                    if (ve == std::string::npos) { value = src.substr(j); j = n; }
                    else { value = src.substr(j, ve - j); j = ve + 1; }
                } else {
                    size_t ve = j;
                    while (ve < n && !is_space(src[ve]) && src[ve] != '>') ++ve;
                    value = src.substr(j, ve - j);
                    j = ve;
                }
                value = decode_entities(value);
            }
            if (!key.empty() && node) node->attrs[key] = value;
        }
        i = j;
        if (self_closed || is_void(tag)) continue;
        if (too_deep) {           // 丢弃整棵超深子树（与 Python 的 0 字符一致）
            skipped_depth++;
            continue;
        }
        // script/style：原始内容整体跳过（内容不是 HTML，也不参与文本）
        if (tag == "script" || tag == "style") {
            std::string close = "</" + tag;
            size_t ci = i;
            while (ci < n) {
                size_t pos;
                // 大小写不敏感地找 "</script"/"</style"
                for (pos = ci; pos + close.size() <= n; ++pos) {
                    size_t k = 0;
                    while (k < close.size() &&
                           to_lower(std::string(1, src[pos + k]))[0] == close[k]) ++k;
                    if (k == close.size()) break;
                }
                if (pos + close.size() > n) { ci = n; break; }
                size_t after = pos + close.size();
                while (after < n && is_space(src[after])) ++after;
                if (after < n && src[after] == '>') { ci = after + 1; break; }
                ci = pos + 1;  // 假闭合（如 "</scriptx"），继续找
            }
            i = ci;
            continue;  // 该元素不入栈（整棵子树等价于被跳过）
        }
        stack.push_back(node);
    }
    return root;
}

// ============================================================ HTML 块规则
const char* const kHeadingLevels[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
const char* const kTextContainers[] = {
    "p", "div", "li", "blockquote", "pre", "td", "th", "figcaption",
    "dd", "dt", "section", "article", "main", "aside", "header", "footer",
    "caption", "address"};
const char* const kBlockLevelExtra[] = {"tr", "table", "ul", "ol", "dl", "figure"};
const char* const kSkipTags[] = {"script", "style", "head", "title", "nav", "noscript",
                                 "template", "iframe", "object", "video", "audio", "math"};

int heading_level_of(const std::string& n) {
    for (int i = 0; i < 6; ++i)
        if (n == kHeadingLevels[i]) return i + 1;
    return 0;
}

bool is_text_container(const std::string& n) {
    for (const char* t : kTextContainers)
        if (n == t) return true;
    return false;
}

bool is_block_level(const std::string& n) {
    if (heading_level_of(n) || is_text_container(n)) return true;
    for (const char* t : kBlockLevelExtra)
        if (n == t) return true;
    return false;
}

bool is_skip_tag(const std::string& n) {
    for (const char* t : kSkipTags)
        if (n == t) return true;
    return false;
}

// CJK 判定（epub.py _CJK_SPACE_RE 的字符类）
inline bool is_cjk_class(char32_t c) {
    return (c >= 0x3000 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
           (c >= 0xFF00 && c <= 0xFFEF);
}

// Python str.strip() 的空白集合（元数据用）
inline bool is_ws_cp(char32_t c) {
    switch (c) {
        case U'\t': case U'\n': case U'\v': case U'\f': case U'\r':
        case U' ': case U'\x85': case U'\xA0': case U'\x1680':
        case U'\x2028': case U'\x2029': case U'\x202F': case U'\x205F': case U'\x3000':
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

// 空白折叠：[ \t\r\n\f\v]+ → 单空格（等价 re.sub(r"\s+", " ", s)）
// 注意**首尾也要各留一个空格**：Python epub.py:482 就是这样处理每个文本节点的，
// 之后 _tidy_spans 只对首个 span 做 lstrip、末个 span 做 rstrip。
// 早期实现在 out 为空时丢掉前导空白，于是 <p>a<b> b</b></p> 会拼成 "ab"、
// <p>x<span> </span>y</p> 会拼成 "xy" —— 词粘连、折行位置与页数全变。
U32Text collapse_ws(const U32Text& in) {
    U32Text out;
    out.reserve(in.size());
    bool in_ws = false;
    for (char32_t c : in) {
        if (c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == U'\f' ||
            c == U'\v') {
            in_ws = true;
            continue;
        }
        if (in_ws) out.push_back(U' ');
        in_ws = false;
        out.push_back(c);
    }
    if (in_ws) out.push_back(U' ');  // 与 re.sub 一致（尾部也替换）
    return out;
}

// CJK 相邻空格删除：(?<=[CJK])[ \t]+(?=[CJK])（折叠后至多单空格）
U32Text remove_cjk_spaces(const U32Text& in) {
    if (in.size() < 3) return in;
    U32Text out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char32_t c = in[i];
        if (c == U' ' && i > 0 && i + 1 < in.size() && is_cjk_class(in[i - 1]) &&
            is_cjk_class(in[i + 1])) {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

U32Text tidy_one(const U32Text& t) { return remove_cjk_spaces(collapse_ws(t)); }

void append_span(std::vector<Span>& spans, U32Text text, SpanStyle style) {
    if (text.empty()) return;
    if (!spans.empty() && spans.back().style == style) {
        spans.back().text += text;
    } else {
        spans.push_back(Span{std::move(text), style});
    }
}

// _tidy_spans：逐 span 折叠 + CJK 删空格；首尾 trim；合并空 span；中间 rstrip
void tidy_spans(std::vector<Span>& spans) {
    for (Span& s : spans) s.text = tidy_one(s.text);
    if (!spans.empty()) {
        // 首 span 去 leading 空格、末 span 去 trailing 空格
        size_t b = 0;
        while (b < spans.front().text.size() && spans.front().text[b] == U' ') ++b;
        spans.front().text = spans.front().text.substr(b);
        while (!spans.back().text.empty() && spans.back().text.back() == U' ')
            spans.back().text.pop_back();
    }
    std::vector<Span> cleaned;
    cleaned.reserve(spans.size());
    for (Span& s : spans)
        if (!s.text.empty()) cleaned.push_back(std::move(s));
    spans = std::move(cleaned);
    for (size_t i = 0; i + 1 < spans.size(); ++i) {
        while (!spans[i].text.empty() && spans[i].text.back() == U' ')
            spans[i].text.pop_back();
    }
}

U32Text spans_text(const std::vector<Span>& spans) {
    U32Text out;
    for (const Span& s : spans) out += s.text;
    return out;
}

SpanStyle style_of(bool bold, bool italic) {
    if (bold && italic) return SpanStyle::BoldItalic;
    if (bold) return SpanStyle::Bold;
    if (italic) return SpanStyle::Italic;
    return SpanStyle::None;
}

// 是否有块级后代（_has_block_descendant）
bool has_block_descendant(const HNode& n) {
    for (const HNode& c : n.children) {
        if (c.is_text) continue;
        if (is_block_level(c.name)) return true;
        if (has_block_descendant(c)) return true;
    }
    return false;
}

// 收集文本，语义 = BeautifulSoup 的 get_text(" ", strip=True)：
// **每个文本节点各自 strip（只去首尾）**，非空片段之间用单个空格连接。
// 注意不能整体 collapse —— 那样 "Chapter    One" 会被压成 "Chapter One"，
// 与 Python 的 nav 目录标题不一致（审查 M7）。Python 的 str.strip() 覆盖 Unicode
// 空白（含 U+3000/U+00A0），这里按同一集合判断。
U32Text gather_all_text(const HNode& n) {
    auto is_uws = [](char32_t c) {
        return c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == U'\f' ||
               c == U'\v' || c == 0x85 || c == 0xA0 || c == 0x3000 ||
               (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
               c == 0x202F || c == 0x205F;
    };
    std::vector<U32Text> parts;
    std::function<void(const HNode&)> walk = [&](const HNode& e) {
        for (const HNode& c : e.children) {
            if (c.is_text) {
                U32Text t = utf8_to_u32(c.text);
                size_t b = 0, en = t.size();
                while (b < en && is_uws(t[b])) ++b;
                while (en > b && is_uws(t[en - 1])) --en;
                if (en > b) parts.push_back(t.substr(b, en - b));
            } else {
                walk(c);
            }
        }
    };
    walk(n);
    U32Text joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) joined.push_back(U' ');
        joined += parts[i];
    }
    return joined;
}

// _append_direct_text：只取容器直接文本子节点，输出单个 paragraph
void append_direct_text(std::vector<Block>& blocks, const HNode& tag) {
    std::vector<U32Text> parts;
    for (const HNode& c : tag.children) {
        if (!c.is_text) continue;
        U32Text t = utf8_to_u32(c.text);
        // strip 后非空才收（Python: if text.strip()）
        U32Text collapsed = collapse_ws(t);
        bool any = false;
        for (char32_t ch : collapsed)
            if (ch != U' ') { any = true; break; }
        if (any) parts.push_back(std::move(t));
    }
    if (parts.empty()) return;
    U32Text joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) joined.push_back(U' ');
        joined += parts[i];
    }
    U32Text text = tidy_one(joined);
    // strip 两端空格
    size_t b = 0, e2 = text.size();
    while (b < e2 && text[b] == U' ') ++b;
    while (e2 > b && text[e2 - 1] == U' ') --e2;
    text = text.substr(b, e2 - b);
    if (text.empty()) return;
    Block blk;
    blk.type = BlockType::Paragraph;
    blk.spans.push_back(Span{std::move(text), SpanStyle::None});
    blocks.push_back(std::move(blk));
}

// _spans：后代文本节点 + br→空格 + img→空格；样式取祖先链（b/strong、i/em）
void collect_spans(const HNode& n, bool bold, bool italic,
                   std::vector<Span>& spans) {
    for (const HNode& c : n.children) {
        if (c.is_text) {
            U32Text t = tidy_one(utf8_to_u32(c.text));
            if (!t.empty()) append_span(spans, std::move(t), style_of(bold, italic));
            continue;
        }
        if (c.name == "br") {
            append_span(spans, U32Text(1, U' '), style_of(bold, italic));
            continue;
        }
        if (c.name == "img") {
            append_span(spans, U32Text(1, U' '), SpanStyle::None);
            continue;
        }
        bool b2 = bold || c.name == "b" || c.name == "strong";
        bool i2 = italic || c.name == "i" || c.name == "em";
        collect_spans(c, b2, i2, spans);
    }
}

}  // namespace

// ============================================================ EpubAdapter
EpubAdapter::EpubAdapter(const AppConfig& cfg, const BookRef& ref, const AdapterEnv& env)
    : cfg_(cfg), ref_(ref), fonts_(env.fonts) {
    format = "epub";
    mode = "flow";
    auto* zip = new mz_zip_archive{};
    if (!mz_zip_reader_init_file(zip, path_to_utf8(ref_.path).c_str(), 0)) {
        delete zip;
        throw ParseError("EPUB 容器无法打开: " + ref_.path.string());
    }
    zip_ = zip;
    int count = mz_zip_reader_get_num_files(zip);
    for (int idx = 0; idx < count; ++idx) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(zip, unsigned(idx), &st)) continue;
        // 条目名解码（flag 0x800 → 原名；否则 cp437 还原）
        std::string name = decode_zip_name(st.m_filename, st.m_bit_flag);
        names_.push_back(name);
        by_name_[name] = idx;
        by_lower_[to_lower(name)].push_back(idx);
        by_base_[to_lower(posix_basename(name))].push_back(idx);
    }
}

EpubAdapter::~EpubAdapter() {
    if (zip_) {
        mz_zip_reader_end(static_cast<mz_zip_archive*>(zip_));
        delete static_cast<mz_zip_archive*>(zip_);
    }
}

// 条目大小上限（压缩包守卫，防 zip 炸弹）
namespace {
struct EpubOut {
    std::string* out = nullptr;
    long long limit = 0;   // ≤0 不限
    bool too_big = false;
};

// miniz 流式回调：返回已消费字节数，返回 0 表示中止。Python 是 read(limit+1) 后
// len(data) > limit 即失败；等价于"已消费总量一旦超过 limit 就中止"。原先允许
// 恰好 limit+1 字节整块通过（差一漏放，审查 M7）
size_t epub_read_cb(void* pOpaque, mz_uint64, const void* pBuf, size_t n) {
    auto* ctx = static_cast<EpubOut*>(pOpaque);
    if (ctx->limit > 0 && (long long)ctx->out->size() + (long long)n > ctx->limit) {
        ctx->too_big = true;
        return 0;
    }
    ctx->out->append(static_cast<const char*>(pBuf), n);
    return n;
}
}  // namespace

std::string EpubAdapter::read_guarded(int index, const std::string& entry) {
    auto* zip = static_cast<mz_zip_archive*>(zip_);
    // 加密条目（general purpose bit0）：miniz 不支持解密，不先说明就只会得到含糊的
    // "条目无法读取"。文案指向 Python zipfile 的 password required（审查 M10）
    mz_zip_archive_file_stat st{};
    if (mz_zip_reader_file_stat(zip, unsigned(index), &st) && st.m_is_encrypted) {
        throw ParseError("EPUB 条目已加密，需要密码: " + entry);
    }
    long long limit = (cfg_.limits.max_archive_entry_mb > 0
                           ? cfg_.limits.max_archive_entry_mb * 1024 * 1024
                           : 0);
    // 流式读取 + 上限回调。**不要**按"声明的解压后大小"提前拒绝：声明可能撒谎
    // （实测：中心目录写 2.75 GiB、实际几百字节的文件，Python 能正常读），而
    // extract_to_callback 是边解压边回调、不会按声明尺寸预分配，超限由回调中止
    // ——安全性由回调"超过 limit 即中止"保证，行为与 Python 的 fh.read(limit+1)
    // 后判 len > limit 一致。
    std::string out;
    EpubOut ctx{&out, limit, false};
    if (!mz_zip_reader_extract_to_callback(zip, unsigned(index), epub_read_cb, &ctx, 0) ||
        ctx.too_big) {
        out.clear();
        if (ctx.too_big && limit > 0) {
            throw ParseError("EPUB 条目超过上限（" +
                             std::to_string(limit / 1024 / 1024) + "MB）: " + entry);
        }
        throw ParseError("EPUB 条目无法读取: " + entry);
    }
    return out;
}

// 依次尝试：原名 / unquote / 去前导 / / normpath / 小写全名 / basename 唯一匹配
std::string EpubAdapter::read_entry(const std::string& name) {
    std::vector<std::string> cands;
    cands.push_back(name);
    cands.push_back(url_unquote(name));
    {
        std::string stripped = name;
        size_t p = stripped.find_first_not_of('/');
        cands.push_back(p == std::string::npos ? std::string() : stripped.substr(p));
    }
    cands.push_back(posix_normpath(name));
    for (const std::string& cand : cands) {
        auto it = by_name_.find(cand);
        if (it != by_name_.end()) return read_guarded(it->second, cand);
    }
    {
        auto it = by_lower_.find(to_lower(name));
        if (it != by_lower_.end() && !it->second.empty())
            // Python 的 _lower_map 同名小写键由后写入者覆盖 → 取目录里最后一个（审查 M8）
            return read_guarded(it->second.back(), it->first);
    }
    // basename 兜底只在唯一匹配时使用（多同名无法消歧）
    std::string base = to_lower(posix_basename(name));
    auto it = by_base_.find(base);
    if (it != by_base_.end()) {
        if (it->second.size() == 1) return read_guarded(it->second.front(), name);
        if (it->second.size() > 1) {
            throw AssetNotFoundError("EPUB 内存在多个同名资源，无法消歧: " + name +
                                     "（" + std::to_string(it->second.size()) + " 个匹配）");
        }
    }
    throw AssetNotFoundError("EPUB 内不存在资源: " + name);
}

std::string EpubAdapter::resolve_href(const std::string& base_item,
                                      const std::string& href_raw) const {
    std::string href = url_unquote(href_raw);
    // strip
    size_t b = href.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = href.find_last_not_of(" \t\r\n");
    href = href.substr(b, e - b + 1);
    if (href.rfind("data:", 0) == 0) return "";
    size_t hash = href.find('#');
    if (hash != std::string::npos) href = href.substr(0, hash);
    if (href.empty()) return "";
    if (href[0] == '/') {
        // 全斜杠 href（如 <img src="/">）：lstrip("/") 得空串，Python normpath("")="."
        //（原先 substr(npos) 抛 out_of_range → 每次 /document 都 500，审查 M4）
        size_t ns = href.find_first_not_of('/');
        return posix_normpath(ns == std::string::npos ? std::string() : href.substr(ns));
    }
    return posix_join_norm(posix_dirname(base_item), href);
}

// ---- pugixml 辅助（禁 DTD 防 XXE）----
static pugi::xml_document parse_xml(const std::string& bytes, bool& ok) {
    pugi::xml_document doc;
    // parse_default（pugixml 默认不解析 DTD 实体，天然防 XXE）
    pugi::xml_parse_result res =
        doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_default);
    ok = res;
    return doc;
}

static std::string local_name(std::string_view n) {
    size_t pos = n.rfind(':');
    return std::string(pos == std::string_view::npos ? n : n.substr(pos + 1));
}

// XML 树遍历的深度上限：pugixml 能解析任意深度的文档，而下面这些遍历是递归的
// （深 OPF/NCX/FB2 同样能打崩进程，审查 B3）。超过上限的分支直接不再下钻。
constexpr int kMaxXmlDepth = 200;

// 深度优先找第一个 localname 匹配的后代（find_local 语义）
static pugi::xml_node find_first_local(const pugi::xml_node& root, const char* local,
                                       int depth = 0) {
    if (depth > kMaxXmlDepth) return {};
    for (pugi::xml_node n = root.first_child(); n; n = n.next_sibling()) {
        if (local_name(n.name()) == local) return n;
        pugi::xml_node sub = find_first_local(n, local, depth + 1);
        if (sub) return sub;
    }
    return {};
}

// 所有 localname 匹配的后代（文档序）
static void find_all_local(const pugi::xml_node& root, const char* local,
                           std::vector<pugi::xml_node>& out, int depth = 0) {
    if (depth > kMaxXmlDepth) return;
    for (pugi::xml_node n = root.first_child(); n; n = n.next_sibling()) {
        if (local_name(n.name()) == local) out.push_back(n);
        find_all_local(n, local, out, depth + 1);
    }
}

// itertext：所有后代文本拼接
static std::string xml_all_text(const pugi::xml_node& n) {
    std::string out;
    std::function<void(const pugi::xml_node&, int)> walk =
        [&](const pugi::xml_node& e, int depth) {
        if (depth > kMaxXmlDepth) return;
        for (pugi::xml_node c = e.first_child(); c; c = c.next_sibling()) {
            if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata)
                out += c.value();
            else
                walk(c, depth + 1);
        }
    };
    walk(n, 0);
    return out;
}

// " ".join(x.split())：折叠所有空白为单空格并 trim
static std::string collapse_xml_text(const std::string& s) {
    U32Text u = utf8_to_u32(s);
    U32Text c = collapse_ws(u);
    size_t b = 0, e = c.size();
    while (b < e && c[b] == U' ') ++b;
    while (e > b && c[e - 1] == U' ') --e;
    return u32_to_utf8(c.substr(b, e - b));
}

// Python xmlutil.text_local / all_text(...).strip()：**只去首尾**，内部空白原样保留。
// 元数据（dc:title / dc:creator / dc:language / NCX 标签）必须用它：
// 用 collapse_xml_text 会把 "My\n  Book" 变成 "My Book"，与 Python 不同。
static std::string strip_xml_text(const std::string& s) {
    U32Text u = utf8_to_u32(s);
    size_t b = 0, e = u.size();
    while (b < e && is_ws_cp(u[b])) ++b;
    while (e > b && is_ws_cp(u[e - 1])) --e;
    return u32_to_utf8(u.substr(b, e - b));
}

// ---- 目录标题 ----
// EPUB3 nav：nav 文档内所有 <a> 的 href/text
static std::map<std::string, std::string> toc_from_nav(const std::string& html,
                                                       const std::string& nav_name) {
    std::map<std::string, std::string> out;
    HNode root = parse_html_tree(html);
    std::function<void(const HNode&)> walk = [&](const HNode& n) {
        for (const HNode& c : n.children) {
            if (c.is_text) continue;
            if (c.name == "a") {
                auto href = c.attrs.find("href");
                U32Text text = gather_all_text(c);
                if (href != c.attrs.end() && !text.empty()) {
                    std::string target = posix_join_norm(
                        posix_dirname(nav_name), url_unquote(href->second).substr(
                                                     0, url_unquote(href->second).find('#')));
                    out.emplace(std::move(target), u32_to_utf8(text));
                }
            }
            walk(c);
        }
    };
    walk(root);
    return out;
}

// NCX：navPoint/navLabel/text + content/src
static std::map<std::string, std::string> toc_from_ncx(const std::string& xml,
                                                       const std::string& ncx_name) {
    std::map<std::string, std::string> out;
    bool ok = false;
    pugi::xml_document doc = parse_xml(xml, ok);
    if (!ok) return out;
    std::vector<pugi::xml_node> nav_points;
    find_all_local(doc, "navPoint", nav_points);
    for (const pugi::xml_node& np : nav_points) {
        pugi::xml_node label = find_first_local(np, "text");
        pugi::xml_node content = find_first_local(np, "content");
        if (!content || std::string(content.attribute("src").as_string()).empty())
            continue;
        std::string src = content.attribute("src").as_string();
        std::string target =
            posix_join_norm(posix_dirname(ncx_name),
                            url_unquote(src).substr(0, url_unquote(src).find('#')));
        if (label) {
            std::string text = strip_xml_text(xml_all_text(label));
            if (!text.empty()) out.emplace(std::move(target), std::move(text));
        }
    }
    return out;
}

// ---- 封面文件名猜测 ----
// 正则 (^|/)cover[^/]*\.(jpe?g|png|gif|webp|bmp)$ IGNORECASE 的手写等价
static bool cover_name_match(const std::string& name) {
    size_t base_start = name.find_last_of('/');
    std::string base = to_lower(base_start == std::string::npos ? name
                                                                : name.substr(base_start + 1));
    static const char* const exts[] = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp"};
    for (const char* e : exts) {
        std::string es(e);
        if (base.size() >= es.size() &&
            base.compare(base.size() - es.size(), es.size(), es) == 0) {
            // stem 必须以 "cover" 开头（对应 (^|/)cover[^/]*\.<ext>$）
            std::string stem = base.substr(0, base.size() - es.size());
            return stem.rfind("cover", 0) == 0;
        }
    }
    return false;
}

static std::optional<std::string> guess_cover(const std::vector<std::string>& image_names) {
    if (image_names.empty()) return std::nullopt;
    for (const std::string& n : image_names)
        if (cover_name_match(n)) return n;
    for (const std::string& n : image_names) {
        std::string base = to_lower(posix_basename(n));
        if (base.find("cover") != std::string::npos) return n;
    }
    return std::nullopt;
}

bool is_image_ext_epub(const std::string& ext) {
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" ||
           ext == ".webp" || ext == ".bmp";
}

// 懒加载模型：容器 + OPF（meta 与 document 共用）
void EpubAdapter::load_model() {
    if (model_) return;
    Model m;
    // 手写 OPF/spine 路径（唯一路径）
    // Python 的判据是"container.xml / OPF 缺失或不可解析 → 失败"，**空 spine 是合法的**
    // （返回 200 + 空章节表）。因此这里记一个 opf_ok，只在 OPF 层面失败时才报错（审查 M5）。
    bool opf_ok = false;
    std::string container_xml;
    bool have_container = true;
    try {
        container_xml = read_entry("META-INF/container.xml");
    } catch (const AssetNotFoundError&) {
        have_container = false;
    }
    if (have_container) {
        bool ok = false;
        pugi::xml_document container = parse_xml(container_xml, ok);
        std::vector<pugi::xml_node> rootfiles;
        if (ok) find_all_local(container, "rootfile", rootfiles);
        std::string opf_path;
        if (!rootfiles.empty())
            opf_path = posix_normpath(rootfiles.front().attribute("full-path").as_string());
        if (!opf_path.empty()) {
            // 图片块的 asset 要按 Python 的口径给"OPF 相对路径"（Python 的 item_name 用的是
            // manifest href，如 text/ch1.xhtml，而不是容器路径 OEBPS/text/ch1.xhtml）：
            // 同一个 href 两边解析出的字符串因此不同，客户端会当成两个 URL（ETag 也随之不同）。
            opf_dir_ = posix_dirname(opf_path);
            std::string opf_bytes;
            bool have_opf = true;
            try {
                opf_bytes = read_entry(opf_path);
            } catch (const AssetNotFoundError&) {
                have_opf = false;
            }
            bool ok2 = false;
            pugi::xml_document opf = parse_xml(opf_bytes, ok2);
            if (have_opf && ok2) {
                opf_ok = true;  // OPF 已成功解析（空 spine 不算失败）
                // DC 元数据（localname 匹配）
                pugi::xml_node t = find_first_local(opf, "title");
                std::string title = t ? strip_xml_text(xml_all_text(t)) : "";
                m.title = title.empty() ? stem_of(ref_.name()) : title;
                pugi::xml_node c = find_first_local(opf, "creator");
                if (c) {
                    std::string a = strip_xml_text(xml_all_text(c));
                    if (!a.empty()) m.author = a;
                }
                pugi::xml_node l = find_first_local(opf, "language");
                if (l) {
                    std::string lg = strip_xml_text(xml_all_text(l));
                    if (!lg.empty()) m.language = lg;
                }
                // manifest：id → href/media-type/properties；manifest_order 保留文档序
                // （nav/NCX 取"第一个"必须按文档序，Python 遍历 dict.values()；审查 M9）
                struct ItemInfo {
                    std::string href;
                    std::string media_type;
                    std::string properties;
                };
                std::map<std::string, ItemInfo> manifest;
                std::vector<std::string> manifest_order;
                std::vector<pugi::xml_node> items;
                find_all_local(opf, "item", items);
                for (const pugi::xml_node& item : items) {
                    std::string id = item.attribute("id").as_string();
                    if (id.empty()) continue;
                    if (manifest.find(id) == manifest.end()) manifest_order.push_back(id);
                    manifest[id] = {item.attribute("href").as_string(),
                                    item.attribute("media-type").as_string(),
                                    item.attribute("properties").as_string()};
                }
                // spine 顺序（仅 application/xhtml*）。spine 全量驻留内存：累计解压
                // 总量受 max_archive_total_mb 约束（与容器一致），防数千个 512MB 级
                // spine 把内存打爆（审查 M6），超限按 Python 的容器文案报 422
                const long long max_total_bytes =
                    cfg_.limits.max_archive_total_mb > 0
                        ? cfg_.limits.max_archive_total_mb * 1024 * 1024
                        : 0;
                long long total_bytes = 0;
                std::vector<pugi::xml_node> itemrefs;
                find_all_local(opf, "itemref", itemrefs);
                for (const pugi::xml_node& ir : itemrefs) {
                    std::string idref = ir.attribute("idref").as_string();
                    auto it = manifest.find(idref);
                    if (it == manifest.end()) continue;
                    if (it->second.media_type.rfind("application/xhtml", 0) != 0) continue;
                    std::string name = posix_join_norm(posix_dirname(opf_path),
                                                       url_unquote(it->second.href));
                    Doc d;
                    d.name = name;
                    try {
                        d.html = read_entry(name);
                    } catch (const AssetNotFoundError&) {
                        continue;
                    }
                    total_bytes += (long long)d.html.size();
                    if (max_total_bytes > 0 && total_bytes > max_total_bytes)
                        throw ParseError(archive_total_limit_error(total_bytes));
                    m.docs.push_back(std::move(d));
                }
                // 目录：EPUB3 nav 优先，其次 NCX（按 manifest 文档序，审查 M9）
                std::map<std::string, std::string> toc_titles;
                for (const std::string& id : manifest_order) {
                    const ItemInfo& info = manifest[id];
                    // properties 含独立词 "nav"
                    std::string props = " " + info.properties + " ";
                    if (props.find(" nav ") == std::string::npos) continue;
                    std::string nav_name =
                        posix_join_norm(posix_dirname(opf_path), url_unquote(info.href));
                    try {
                        toc_titles = toc_from_nav(read_entry(nav_name), nav_name);
                    } catch (...) {
                    }
                    break;
                }
                if (toc_titles.empty()) {
                    for (const std::string& id : manifest_order) {
                        const ItemInfo& info = manifest[id];
                        if (info.media_type != "application/x-dtbncx+xml") continue;
                        std::string ncx_name = posix_join_norm(posix_dirname(opf_path),
                                                               url_unquote(info.href));
                        try {
                            toc_titles = toc_from_ncx(read_entry(ncx_name), ncx_name);
                        } catch (...) {
                        }
                        break;
                    }
                }
                for (Doc& d : m.docs) {
                    auto it = toc_titles.find(d.name);
                    if (it != toc_titles.end()) d.title = it->second;
                }
            }
        }
    }
    if (m.title.empty()) m.title = stem_of(ref_.name());
    if (!opf_ok) {
        mz_zip_reader_end(static_cast<mz_zip_archive*>(zip_));
        throw ParseError("EPUB 结构无法解析（缺少 OPF / spine）");
    }
    // 图片名（sorted() 普通字典序）
    for (const std::string& n : names_)
        if (is_image_ext_epub(ext_of(n))) m.image_names.push_back(n);
    std::sort(m.image_names.begin(), m.image_names.end());
    m.cover_name = guess_cover(m.image_names);
    model_ = std::move(m);
}

const EpubAdapter::Model& EpubAdapter::model() {
    load_model();
    return *model_;
}

AdapterMeta EpubAdapter::meta() {
    const Model& m = model();
    AdapterMeta meta;
    meta.title = m.title.empty() ? stem_of(ref_.name()) : m.title;
    meta.format = format;
    meta.mode = mode;  // "flow"
    meta.author = m.author;
    meta.language = m.language;
    meta.size = ref_.size;
    meta.page_count = std::nullopt;
    for (size_t i = 0; i < m.docs.size(); ++i) {
        const Doc& d = m.docs[i];
        std::string title = d.title.empty() ? stem_of(d.name) : d.title;
        meta.chapters.push_back(nlohmann::json{{"index", i},
                                               {"title", title},
                                               {"total_chars", nullptr}});
    }
    meta.supports_json = true;
    return meta;
}

// ---- HTML → 块 ----
// src/data-src/xlink:href/属性名以 href 结尾 → 容器内路径（空串 = 无 / 不可用）
std::string epub_image_resolved(EpubAdapter& self, const HNode& tag,
                                const std::string& item_name) {
    std::string src;
    auto get = [&](const char* k) -> std::string {
        auto it = tag.attrs.find(k);
        return it == tag.attrs.end() ? std::string() : it->second;
    };
    src = get("src");
    if (src.empty()) src = get("data-src");
    if (src.empty()) src = get("xlink:href");
    if (src.empty()) {
        // 属性名以 href 结尾（任意命名空间写法）
        for (const auto& [k, v] : tag.attrs) {
            if (k.size() >= 4 && k.compare(k.size() - 4, 4, "href") == 0) {
                src = v;
                break;
            }
        }
    }
    // strip
    size_t b = src.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = src.find_last_not_of(" \t\r\n");
    src = src.substr(b, e - b + 1);
    if (src.empty()) return "";
    if (src.rfind("data:", 0) == 0) return "";  // data: URI 无法经 /asset 提供
    return self.resolve_href_pub(item_name, src);
}

// _image_block：解析结果 → image 块
std::optional<Block> epub_image_block(EpubAdapter& self, const HNode& tag,
                                      const std::string& item_name) {
    std::string resolved = epub_image_resolved(self, tag, item_name);
    if (resolved.empty()) return std::nullopt;
    auto [w, h] = self.image_size_pub(resolved);
    Block blk;
    blk.type = BlockType::Image;
    // 与 Python 一致：asset 用 OPF 相对路径（见 opf_relative 注释）
    blk.asset = self.opf_relative(resolved);
    blk.w = w;
    blk.h = h;
    return blk;
}

// 遍历（等价 find_all(True) 的文档序逐元素处理；跳过子树 = 逐元素 in_skipped 的等价）。
// image_paths 非空时按出现顺序收集图片的容器内路径（图片型检测用）。
void epub_walk(EpubAdapter& self, const HNode& n, const std::string& item_name,
               std::vector<Block>& blocks, std::vector<std::string>* image_paths) {
    for (const HNode& c : n.children) {
        if (c.is_text) continue;
        if (is_skip_tag(c.name)) continue;  // 整棵子树跳过
        int lvl = heading_level_of(c.name);
        if (lvl) {
            if (has_block_descendant(c)) {
                // 含块级后代：只取直接文本
                append_direct_text(blocks, c);
            } else {
                std::vector<Span> spans;
                collect_spans(c, false, false, spans);
                tidy_spans(spans);
                U32Text text = spans_text(spans);
                bool any = false;
                for (char32_t ch : text)
                    if (ch != U' ') { any = true; break; }
                if (any) {
                    Block blk;
                    blk.type = BlockType::Heading;
                    blk.level = lvl;
                    blk.spans = std::move(spans);
                    blocks.push_back(std::move(blk));
                }
            }
        } else if (is_text_container(c.name)) {
            if (has_block_descendant(c)) {
                // 容器自己的直接文本也保留（顺序在子块之前）
                append_direct_text(blocks, c);
            } else {
                std::vector<Span> spans;
                collect_spans(c, false, false, spans);
                tidy_spans(spans);
                U32Text text = spans_text(spans);
                bool any = false;
                for (char32_t ch : text)
                    if (ch != U' ') { any = true; break; }
                if (any) {
                    Block blk;
                    blk.type = BlockType::Paragraph;
                    blk.spans = std::move(spans);
                    blocks.push_back(std::move(blk));
                }
            }
        } else if (c.name == "hr") {
            Block blk;
            blk.type = BlockType::Separator;
            blocks.push_back(std::move(blk));
        } else if (c.name == "img" || c.name == "image") {
            if (image_paths != nullptr) {
                std::string resolved = epub_image_resolved(self, c, item_name);
                if (!resolved.empty()) image_paths->push_back(std::move(resolved));
            }
            if (auto blk = epub_image_block(self, c, item_name)) {
                blocks.push_back(std::move(*blk));
            }
        }
        // 继续遍历后代（与 find_all(True) 一致）
        epub_walk(self, c, item_name, blocks, image_paths);
    }
}

std::vector<Block> EpubAdapter::html_to_blocks(const std::string& html,
                                               const std::string& item_name,
                                               std::vector<std::string>* image_paths) {
    HNode root = parse_html_tree(html);
    // 等价 soup.body if soup.body is not None else soup：找首个 body 元素
    const HNode* body = &root;
    std::function<const HNode*(const HNode&)> find_body = [&](const HNode& n) -> const HNode* {
        for (const HNode& c : n.children) {
            if (!c.is_text && c.name == "body") return &c;
            if (const HNode* r = find_body(c)) return r;
        }
        return nullptr;
    };
    if (const HNode* b = find_body(root)) body = b;
    std::vector<Block> blocks;
    epub_walk(*this, *body, item_name, blocks, image_paths);
    return blocks;
}

// len(block.text.strip())：Python str.strip() 去首尾 Unicode 空白（含全角空格）
static long long stripped_text_len(const U32Text& text) {
    auto is_ws = [](char32_t c) {
        return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\x0b' ||
               c == U'\x0c' || c == U'\u3000';
    };
    size_t b = 0, e = text.size();
    while (b < e && is_ws(text[b])) ++b;
    while (e > b && is_ws(text[e - 1])) --e;
    return (long long)(e - b);
}

// 图片型检测：与 Python imagebook.scan_image_book 同一判据。
// 预筛用内存中的 spine 字节数（文档在 meta 阶段已读入），避免文本 EPUB
// 在 /meta 阶段被全量解析成块。
ImageBookInfo EpubAdapter::scan_image_book() {
    const Model& m = model();
    ImageBookInfo out;
    out.title = m.title.empty() ? stem_of(ref_.name()) : m.title;
    if ((int)m.image_names.size() < kImageBookMinPages) return out;
    long long xhtml_bytes = 0;
    for (const Doc& d : m.docs) xhtml_bytes += (long long)d.html.size();
    if (xhtml_bytes > kImageBookXhtmlBytesPerImage * (long long)m.image_names.size()) return out;
    for (const Doc& d : m.docs) {
        std::vector<Block> blocks = html_to_blocks(d.html, d.name, &out.pages);
        for (const Block& b : blocks) {
            if (b.type != BlockType::Heading && b.type != BlockType::Paragraph) continue;
            out.text_chars += stripped_text_len(b.text());
        }
    }
    out.manga = (int)out.pages.size() >= kImageBookMinPages &&
                out.text_chars <= (long long)kImageBookMaxTextPerPage * (long long)out.pages.size();
    return out;
}

std::string EpubAdapter::read_entry_pub(const std::string& name) { return read_entry(name); }

std::pair<int, int> EpubAdapter::image_size(const std::string& name) {
    auto it = size_cache_.find(name);
    if (it != size_cache_.end()) return it->second;
    std::pair<int, int> size{600, 800};  // probe 失败默认
    try {
        std::string data = read_entry(name);
        auto probed = probe_size(data);
        if (probed.first > 0 && probed.second > 0) size = probed;
    } catch (...) {
    }
    size_cache_[name] = size;
    return size;
}

// 供 epub_image_block / epub_walk 使用的公开转发（定义于类外辅助函数）
std::string EpubAdapter::resolve_href_pub(const std::string& base, const std::string& href) {
    return resolve_href(base, href);
}

// 容器路径 → OPF 相对路径：把 OPF 目录前缀去掉（Python 的 href 解析基准是 manifest href）。
// 前缀不匹配时原样返回（例如 href 用绝对路径 / 或跨越 OPF 目录的极端写法）。
std::string EpubAdapter::opf_relative(const std::string& container_path) const {
    if (opf_dir_.empty() || opf_dir_ == ".") return container_path;
    std::string prefix = opf_dir_ + "/";
    if (container_path.rfind(prefix, 0) == 0)
        return container_path.substr(prefix.size());
    return container_path;
}
std::pair<int, int> EpubAdapter::image_size_pub(const std::string& name) {
    return image_size(name);
}

DocumentModel EpubAdapter::document() {
    const Model& m = model();
    DocumentModel doc;
    doc.book_id = ref_.book_id;
    doc.format = format;
    size_t total_chars = 0;
    for (size_t i = 0; i < m.docs.size(); ++i) {
        const Doc& d = m.docs[i];
        Chapter ch;
        ch.index = int(i);
        ch.blocks = html_to_blocks(d.html, d.name);
        // 章标题：toc → 首个非空 heading → 文件名 stem
        U32Text title = utf8_to_u32(d.title);
        if (title.empty()) {
            for (const Block& b : ch.blocks) {
                if (b.type != BlockType::Heading) continue;
                U32Text t = b.text();
                bool any = false;
                for (char32_t c2 : t)
                    if (c2 != U' ') { any = true; break; }
                if (any) { title = t; break; }
            }
        }
        if (title.empty()) title = utf8_to_u32(stem_of(d.name));
        ch.title = title;
        total_chars += ch.total_chars();
        doc.chapters.push_back(std::move(ch));
    }
    doc.meta["title"] = m.title.empty() ? stem_of(ref_.name()) : m.title;
    doc.meta["author"] = m.author ? nlohmann::json(*m.author) : nlohmann::json(nullptr);
    doc.meta["language"] =
        m.language ? nlohmann::json(*m.language) : nlohmann::json(nullptr);
    doc.meta["total_chars"] = total_chars;
    doc.meta["source"] = ref_.vpath;
    doc.meta["parser"] = "zip+opf";
    return doc;
}

ImageBytes EpubAdapter::asset(const std::string& path) {
    // posixpath.normpath(path.replace("\\","/").lstrip("/"))
    std::string norm = path;
    for (char& c : norm)
        if (c == '\\') c = '/';
    size_t p = norm.find_first_not_of('/');
    norm = p == std::string::npos ? std::string() : norm.substr(p);
    norm = posix_normpath(norm);
    std::string data = read_entry(norm);
    std::string mime = sniff_mime(data);
    if (mime == "application/octet-stream") mime = mime_from_ext(ext_of(norm));
    return ImageBytes{std::move(data), std::move(mime)};
}

std::optional<ImageBytes> EpubAdapter::cover(int width) {
    const Model* mp = nullptr;
    try {
        mp = &model();
    } catch (const ParseError&) {
        // 对齐 Python epub.py:496-522：模型解析失败的坏书也出文字封面（而不是把
        // ParseError 抛给 /cover 变 422）。字体管理器来自 AdapterEnv（AppContext
        // 的实例），与 /cover 主路径共用同一份，不再建进程级静态 FontManager。
        // 标题/作者与 Python 分支一致：stem + None。
        if (!fonts_) throw ParseError("文字封面缺少字体管理器");
        return text_cover(stem_of(ref_.name()), std::nullopt,
                          width > 0 ? width : 320, fonts_);
    }
    std::optional<std::string> name = mp->cover_name;
    if (!name && !mp->image_names.empty()) name = mp->image_names.front();  // 任何图片第一个
    if (name) {
        try {
            std::string data = read_entry(*name);
            if (width <= 0) {
                std::string mime = sniff_mime(data);
                if (mime == "application/octet-stream") mime = mime_from_ext(ext_of(*name));
                return ImageBytes{std::move(data), std::move(mime)};
            }
            try {
                ImageBuf img = decode_image(data, cfg_.limits.max_source_pixels);
                if (img.w > width) img = resize_image(img, width, 0);
                return encode_image(img, EncodeOpts{.prefer_jpeg = true, .jpeg_quality = 84});
            } catch (...) {
                // 解码失败退原图
                std::string mime = sniff_mime(data);
                if (mime == "application/octet-stream") mime = mime_from_ext(ext_of(*name));
                return ImageBytes{std::move(data), std::move(mime)};
            }
        } catch (const AssetNotFoundError&) {
            // 落到 nullopt
        }
    }
    return std::nullopt;  // 无封面 → 服务层退回文字封面
}

}  // namespace lnv

namespace lnv {


std::unique_ptr<BookAdapter> create_epub_adapter(const AppConfig& cfg, const BookRef& ref,
                                                 const AdapterEnv& env) {
    // 图片型（漫画，每页一图）→ 固定版式漫画管线（铺满屏幕）；文本型维持流式。
    // 检测有进程内缓存；预筛保证文本 EPUB 的 /meta 不做全量 HTML 解析。
    auto flow = std::make_unique<EpubAdapter>(cfg, ref, env);
    ImageBookInfo info = image_book_info(*flow);
    if (!info.manga) return flow;
    return make_epub_comic_adapter(cfg, ref, std::move(flow), info);
}

}  // namespace lnv
