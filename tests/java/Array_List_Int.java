// Java program to illustrate creating an array 
// of integers, puts some values in the array, 
// and prints each value to standard output. 

import java.io.*; 
import java.lang.*; 
import java.util.Scanner;
import java.lang.management.ManagementFactory;
import java.lang.management.MemoryPoolMXBean;
import java.util.LinkedList;
import java.util.ArrayList;

import java.lang.reflect.Field;

public class Array_List_Int { 
	private static final sun.misc.Unsafe _UNSAFE;

	static {
		try {
			Field unsafeField = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
			unsafeField.setAccessible(true);
			_UNSAFE = (sun.misc.Unsafe) unsafeField.get(null);
		} catch (Exception e) {
			throw new RuntimeException("SimplePartition: Failed to " + "get unsafe", e);
		}
	}

	public static void mem_info(String str) {
		System.out.println("=========================================");
		System.out.println(str + "\n");
		System.out.println("=========================================");
		for(MemoryPoolMXBean memoryPoolMXBean: ManagementFactory.getMemoryPoolMXBeans()){
			System.out.println(memoryPoolMXBean.getName());
			System.out.println(memoryPoolMXBean.getUsage().getUsed());
		}
	}

	public static void gc() {
		System.out.println("=========================================");
		System.out.println("Call GC");
		System.gc();
		System.out.println("=========================================");
	}

	public static void calcHashCode(ArrayList<Integer> arl, int num_elements) {
		long sum = 0;

		for (int i = 0; i < num_elements; i++)
			sum += arl.get(i).hashCode();

		System.out.println("Hashcode Element = " + sum);
	}

	public static void main (String[] args) 
	{		 
		int num_elements =10000000;
		long sum = 0;

		mem_info("Memory Before");

		ArrayList<Integer> arl = new ArrayList<Integer>();
		_UNSAFE.tcMarkObjectWithId(arl, 0, 0);

		for (int i = 0; i < num_elements; i++)
			arl.add(new Integer(i));

		gc();
		calcHashCode(arl, num_elements);

        gc();
		calcHashCode(arl, num_elements);

		gc();
		calcHashCode(arl, num_elements);

		arl = null;
		gc();

		ArrayList<Integer> arl2 = new ArrayList<Integer>();
		_UNSAFE.tcMarkObjectWithId(arl2, 1, 0);

		for (int i = 0; i < num_elements; i++)
			arl2.add(new Integer(i));
		
		gc();

		calcHashCode(arl2, num_elements);

		mem_info("Memory After");
	} 
}
