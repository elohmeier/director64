// Original host validation/raster adapter. Uses the project's pinned FreeType
// amalgamation, the same rasterizer source consumed by libdragon mkfont.
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include "FreeTypeAmalgam.h"
static void check(FT_Error e,const char*what){if(e)throw std::runtime_error(std::string(what)+": "+std::to_string(e));}
int main(int argc,char**argv){try{
 if(argc!=3)throw std::runtime_error("usage: font-glyphs FONT.otf POINT_SIZE");
 int size=std::stoi(argv[2]);if(size<1||size>256)throw std::runtime_error("invalid size");
 FT_Library lib;check(FT_Init_FreeType(&lib),"FreeType init");FT_Face face;
 check(FT_New_Face(lib,argv[1],0,&face),"FreeType font");
 FT_Size_RequestRec req{};req.type=FT_SIZE_REQUEST_TYPE_NOMINAL;req.height=size<<6;
 check(FT_Request_Size(face,&req),"FreeType size");
 std::cout<<"{\"size\":"<<size<<",\"unitsPerEm\":"<<face->units_per_EM<<",\"ascender26_6\":"<<face->size->metrics.ascender<<",\"descender26_6\":"<<face->size->metrics.descender<<",\"glyphs\":[";
 FT_UInt index;FT_ULong cp=FT_Get_First_Char(face,&index);bool comma=false;
 while(index){
  check(FT_Load_Glyph(face,index,FT_LOAD_RENDER),"FreeType glyph");
  auto&s=*face->glyph;auto&b=s.bitmap;
  if(b.pixel_mode!=FT_PIXEL_MODE_GRAY && b.rows*b.width)throw std::runtime_error("non-grayscale glyph");
  if(comma)std::cout<<',';comma=true;
  std::cout<<"{\"codepoint\":"<<cp<<",\"glyphIndex\":"<<index<<",\"advance26_6\":"<<s.advance.x<<",\"left\":"<<s.bitmap_left<<",\"top\":"<<s.bitmap_top<<",\"width\":"<<b.width<<",\"height\":"<<b.rows<<",\"coverageHex\":\"";
  for(unsigned y=0;y<b.rows;y++)for(unsigned x=0;x<b.width;x++)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)b.buffer[y*b.pitch+x];
  std::cout<<std::dec<<"\"}";cp=FT_Get_Next_Char(face,cp,&index);
 }
 std::cout<<"]}\n";FT_Done_Face(face);FT_Done_FreeType(lib);return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
