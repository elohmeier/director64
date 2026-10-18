package android.util;
import java.util.TreeMap;
public class SparseIntArray {
  private final TreeMap<Integer,Integer> m=new TreeMap<>();
  public void put(int k,int v){m.put(k,v);}
  public int get(int k){return get(k,0);}
  public int get(int k,int d){return m.getOrDefault(k,d);}
  public int indexOfKey(int k){return m.containsKey(k)?0:-1;}
  public void delete(int k){m.remove(k);}
  public int size(){return m.size();}
  public int keyAt(int i){return (int)m.keySet().toArray()[i];}
  public int valueAt(int i){return (int)m.values().toArray()[i];}
}
