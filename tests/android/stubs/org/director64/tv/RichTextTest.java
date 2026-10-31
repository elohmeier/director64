package org.director64.tv;

public final class RichTextTest {
  private static void check(boolean condition) {if(!condition)throw new AssertionError();}
  private static int[] pack(int[] styles,int flags) {
    int[] result=new int[styles.length/9*10];
    for(int at=0;at<styles.length;at+=9) {
      System.arraycopy(styles,at,result,at/9*10,9);result[at/9*10+9]=flags;
    }
    return result;
  }
  private static RichText.Plan layout(String text,int width,int[] styles,RichText.Measure measure) {
    return RichText.layout(text,width,pack(styles,0),measure);
  }
  public static void main(String[] args) {
    int[] styles={0,1,12,0,1,11,14,0xff0000,0,2,2,24,3,1,21,28,0x0000ff,0};
    RichText.Measure measure=(value,style)->value.codePointCount(0,value.length())*(style.font==1?4:8);
    RichText.Plan plan=layout("ABCD EF",40,styles,measure);
    check(plan.height==56&&plan.parts.size()==3);
    check(plan.parts.get(0).style.font==1&&plan.parts.get(0).style.size==12&&plan.parts.get(0).style.color==0xff0000);
    check(plan.parts.get(1).style.font==2&&plan.parts.get(1).style.face==3&&plan.parts.get(1).style.color==0x0000ff);
    check(plan.parts.get(0).baseline==21&&plan.parts.get(1).baseline==21);
    check(plan.boxes[0]==8&&plan.boxes[3]==28&&plan.boxes[5*4]==12&&plan.boxes[5*4+1]==28);
    RichText.Plan newline=layout("a\r\nb",40,new int[]{0,1,12,0,0,11,14,0,0},measure);
    check(newline.height==28&&newline.boxes[3*4+1]==14&&newline.boxes[1*4+3]==0&&newline.boxes[2*4+3]==0);
    RichText.Plan unicode=layout("\ud83d\ude00\ud83d\ude00",1,new int[]{0,1,12,0,0,11,14,0,0},measure);
    check(unicode.parts.size()==2&&unicode.parts.get(0).text.length()==2&&unicode.parts.get(1).text.length()==2);
    check(unicode.boxes[1]==unicode.boxes[5]&&unicode.boxes[2*4+1]==14&&unicode.boxes[3*4+1]==14);
    RichText.Plan tracking=layout("ABC",100,new int[]{0,1,12,0,0,11,32,0,2},measure);
    check(tracking.boxes[4]==6&&tracking.boxes[8]==12&&tracking.height==32);
    RichText.Plan range=layout("ABC",100,new int[]{0,1,12,0,0,11,14,0,0,1,1,12,0,0,11,14,0,4,2,1,12,0,0,11,14,0,0},measure);
    check(range.boxes[4]==4&&range.boxes[8]==12);
    // Only wrapped nonfinal paragraph rows expand interword ASCII spaces.
    int[] justified={0,1,12,0,3,11,14,0,0};
    RichText.Plan full=layout("A B C D",22,justified,measure);
    check(full.boxes[0]==0&&full.boxes[1*4]==4&&full.boxes[1*4+2]==5);
    check(full.boxes[2*4]==9&&full.boxes[3*4+2]==5&&full.boxes[4*4]==18);
    check(full.boxes[5*4]==22&&full.boxes[5*4+2]==4); // trailing space is not padded
    check(full.boxes[6*4]==0&&full.boxes[6*4+1]==14); // final row stays left
    RichText.Plan finalRow=layout("A B\rC D",40,justified,measure);
    check(finalRow.boxes[1*4+2]==4&&finalRow.boxes[5*4+2]==4);
    check(finalRow.boxes[4*4]==0&&finalRow.boxes[4*4+1]==14);
    int[] mixed={0,1,12,0,3,11,14,0xff0000,1,2,2,24,3,3,21,28,0x0000ff,2};
    RichText.Plan mixedFull=layout("A B C",28,mixed,measure);
    check(mixedFull.boxes[2*4]==18&&mixedFull.boxes[1*4+2]==13);
    check(mixedFull.parts.get(0).baseline==21&&mixedFull.parts.get(2).baseline==21);
    check(mixedFull.parts.get(2).style.font==2&&mixedFull.parts.get(2).style.face==3);
    check(mixedFull.boxes[4*4]==0&&mixedFull.boxes[4*4+1]==28);
    RichText.Plan tabs=layout("A\tB C",14,justified,measure);
    check(tabs.boxes[1*4+2]==4); // tabs do not receive space padding
    RichText.Plan unwrapped=RichText.layout("A B C\rD E",8,pack(justified,1),measure);
    check(unwrapped.height==28&&unwrapped.parts.get(0).text.equals("A B C"));
    check(unwrapped.boxes[4*4]==16&&unwrapped.boxes[4*4+1]==0);
    check(unwrapped.boxes[6*4]==0&&unwrapped.boxes[6*4+1]==14);
    boolean badFlags=false;
    try{RichText.layout("x",8,pack(justified,4),measure);}catch(IllegalArgumentException e){badFlags=true;}
    check(badFlags);
    RichText.Plan classic=RichText.layout("AB\rCD",40,pack(new int[]{0,1,12,0,0,11,15,0,0},2),measure);
    check(classic.height==30&&classic.boxes[3]==14&&classic.boxes[3*4+1]==15);
    check(classic.boxes[3*4+1]+classic.boxes[3*4+3]-1==28);
    RichText.Plan fixedClassic=RichText.layout("AB\rCD",40,pack(new int[]{0,1,12,0,0,11,32,0,0},2),measure);
    check(fixedClassic.height==64&&fixedClassic.boxes[3]==14&&fixedClassic.boxes[3*4+1]==32);
    RichText.Plan negative=layout("A".repeat(70),100,new int[]{0,1,12,0,0,11,15,0,-1000},measure);
    check(negative.boxes[4]==-996&&negative.boxes[34*4]==Short.MIN_VALUE);
    check(negative.boxes[69*4]==Short.MIN_VALUE&&negative.boxes[69*4+2]==-996);
    RichText.Measure originalAdvance=(value,style)->{
      float total=0;for(char c:value.toCharArray())total+=c=='C'||c=='D'?13:12;return total;
    };
    RichText.Plan caret=layout("ABCD",100,new int[]{0,1,18,0,0,17,21,0,-1000},originalAdvance);
    check(caret.boxes[4]==-988&&caret.boxes[8]==-1976&&caret.boxes[12]==-2963);
    check(caret.boxes[12]+caret.boxes[14]==-3950);
    boolean rejected=false;try{layout("x",40,new int[]{1,1,12,0,0,11,14,0,0},measure);}catch(IllegalArgumentException error){rejected=true;}
    check(rejected);
    System.out.println("Android rich text: per-run font/face/color, shared baselines, wrap, CRLF and UTF16 pairs pass");
  }
}
