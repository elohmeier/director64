package org.director64.tv;

import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** UTF16 character ranges and baseline layout shared by rich rasterization and hit testing. */
final class RichText {
  interface Measure { float width(String text, Style style); }
  static final class Style {
    final int offset, font, size, face, align, ascent, lineHeight, color, charSpacing, layoutFlags;
    Style(int[] data, int at) {
      offset=data[at];font=data[at+1];size=data[at+2];face=data[at+3];align=data[at+4];
      ascent=data[at+5];lineHeight=data[at+6];color=data[at+7];charSpacing=data[at+8];
      layoutFlags=data[at+9];
    }
  }
  static final class Part {
    final int start, end;
    final String text;
    final Style style;
    final float width;
    boolean expandedSpace;
    float x;
    int y, baseline, height;
    Part(int start,int end,String text,Style style,float width) {
      this.start=start;this.end=end;this.text=text;this.style=style;this.width=width;
    }
  }
  static final class Plan {
    final List<Part> parts;
    final short[] boxes;
    final int height;
    Plan(List<Part> parts,short[] boxes,int height) {this.parts=parts;this.boxes=boxes;this.height=height;}
  }
  private final String text;
  private final int width;
  private final Measure measure;
  private final List<Style> styles=new ArrayList<>();
  private final List<Part> all=new ArrayList<>();
  private int y;
  private RichText(String text,int width,int[] packed,Measure measure) {
    if(packed.length==0 || packed.length%10!=0 || packed.length/10>4096)throw new IllegalArgumentException("invalid formatted text styles");
    this.text=text;this.width=width;this.measure=measure;
    int previous=-1;
    for(int at=0;at<packed.length;at+=10) {
      Style s=new Style(packed,at);
      if(s.offset<=previous || (at==0&&s.offset!=0) || s.offset>text.length() || s.size<0 || s.align<0 || s.align>3 || (s.layoutFlags&~3)!=0)
        throw new IllegalArgumentException("invalid formatted text range");
      styles.add(s);previous=s.offset;
    }
  }
  private int at(int offset) {
    int i=0;while(i+1<styles.size()&&styles.get(i+1).offset<=offset)i++;return i;
  }
  private List<Part> fragments(int start,int end) {
    List<Part> out=new ArrayList<>();int i=at(start);
    while(start<end) {
      int stop=Math.min(end,i+1<styles.size()?styles.get(i+1).offset:end);
      if(stop<=start){i++;continue;}
      Style s=styles.get(i);String value=text.substring(start,stop);
      out.add(new Part(start,stop,value,s,measure.width(value,s)+s.charSpacing*value.codePointCount(0,value.length())));start=stop;i++;
    }
    return out;
  }
  private float length(int start,int end) {
    float total=0;for(Part p:fragments(start,end))total+=p.width;return total;
  }
  private int trimmed(int start,int end) {
    while(end>start&&Character.isWhitespace(text.charAt(end-1)))end--;return end;
  }
  private Part fragment(int start,int end,Style style,float extra) {
    String value=text.substring(start,end);
    Part result=new Part(start,end,value,style,measure.width(value,style)
        +style.charSpacing*value.codePointCount(0,value.length())+extra);
    result.expandedSpace=extra>0;
    return result;
  }
  private void row(int start,int end,boolean paragraphEnd) {
    List<Part> parts=fragments(start,end);Style base=styles.get(at(start));
    int ascent=0,height=0;
    if(parts.isEmpty()){ascent=base.ascent>0?base.ascent:Math.round((base.size==0?12:base.size)*.8f);height=base.lineHeight>0?base.lineHeight:Math.round((base.size==0?12:base.size)*1.2f);}
    for(Part p:parts) {
      ascent=Math.max(ascent,p.style.ascent>0?p.style.ascent:Math.round((p.style.size==0?12:p.style.size)*.8f));
      height=Math.max(height,p.style.lineHeight>0?p.style.lineHeight:Math.round((p.style.size==0?12:p.style.size)*1.2f));
    }
    int shownEnd=trimmed(start,end),firstWord=start;
    while(firstWord<shownEnd&&Character.isWhitespace(text.charAt(firstWord)))firstWord++;
    float shown=length(start,shownEnd);
    if(base.align==3&&!paragraphEnd&&shown<width) {
      int spaces=0;
      for(int i=firstWord;i<shownEnd;i++)if(text.charAt(i)==' ')spaces++;
      if(spaces>0) {
        float extra=(width-shown)/spaces;
        List<Part> padded=new ArrayList<>();
        for(Part p:parts) {
          int piece=p.start;
          for(int i=p.start;i<p.end;i++)if(i>=firstWord&&i<shownEnd&&text.charAt(i)==' ') {
            if(piece<i)padded.add(fragment(piece,i,p.style,0));
            padded.add(fragment(i,i+1,p.style,extra));piece=i+1;
          }
          if(piece<p.end)padded.add(fragment(piece,p.end,p.style,0));
        }
        parts=padded;
      }
    }
    float x=base.align==1?(width-shown)/2:base.align==2?width-shown:0;
    int cellHeight=(base.layoutFlags&2)!=0?ascent+3:height;
    for(Part p:parts){p.x=x;p.y=y;p.baseline=y+ascent;p.height=cellHeight;x+=p.width;all.add(p);}
    y+=height;
  }
  static Plan layout(String text,int width,int[] packed,Measure measure) {
    return new RichText(text,width,packed,measure).layout();
  }
  private static short bounded(float value) {
    return (short)Math.max(Short.MIN_VALUE,Math.min(Short.MAX_VALUE,Math.round(value)));
  }
  private Plan layout() {
    int paragraph=0;
    Pattern words=Pattern.compile("\\S+\\s*|\\s+");
    while(true) {
      int end=paragraph;while(end<text.length()&&text.charAt(end)!='\r'&&text.charAt(end)!='\n')end++;
      int line=paragraph,cursor=paragraph;
      if((styles.get(at(paragraph)).layoutFlags&1)!=0) {
        row(paragraph,end,true);
        if(end>=text.length())break;
        paragraph=end+(text.startsWith("\r\n",end)?2:1);continue;
      }
      Matcher tokens=words.matcher(text.substring(paragraph,end));
      while(tokens.find()) {
        int stop=cursor+tokens.group().length(),shownEnd=trimmed(line,stop);
        if(line<cursor&&length(line,shownEnd)>width){row(line,cursor,false);line=cursor;}
        while(line<shownEnd&&length(line,shownEnd)>Math.max(1,width)) {
          int fit=line,probe=line;
          while(probe<shownEnd) {
            probe+=Character.charCount(text.codePointAt(probe));
            if(length(line,probe)>Math.max(1,width)&&fit>line)break;
            fit=probe;
          }
          if(fit>=shownEnd)break;
          row(line,fit,false);line=fit;
        }
        cursor=stop;
      }
      row(line,end,true);
      if(end>=text.length())break;
      paragraph=end+(text.startsWith("\r\n",end)?2:1);
    }
    short[] boxes=new short[text.length()*4];
    for(Part p:all)for(int k=0;k<p.text.length();) {
      int count=Character.charCount(p.text.codePointAt(k));
      int ordinal=p.text.codePointCount(0,k);
      float left=p.x+measure.width(p.text.substring(0,k),p.style)+p.style.charSpacing*ordinal;
      float right=p.x+measure.width(p.text.substring(0,k+count),p.style)+p.style.charSpacing*(ordinal+1);
      if(p.expandedSpace){left=p.x;right=p.x+p.width;}
      for(int u=0;u<count;u++) {
        int at=(p.start+k+u)*4;
        boxes[at]=bounded(left);boxes[at+1]=bounded(p.y);
        boxes[at+2]=bounded(right-left);boxes[at+3]=bounded(p.height);
      }
      k+=count;
    }
    return new Plan(all,boxes,y);
  }
}
