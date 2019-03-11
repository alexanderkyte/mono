using System;

class Foo {

        static public int i = 0;

        static Foo () {
                Console.WriteLine ("Foo static ctor ran");
        }
}

class Bar {

        static public int j;

        static Bar () {
                Console.WriteLine ("Bar static ctor ran");
                j = Foo.i;
                GC.GetGeneration ((object) Foo.i);
        }
}

class Bug {

        static public int Main () {
                Foo.i = 1;
                GC.GetGeneration ((object) Foo.i);
                Foo.i = 5;
                GC.GetGeneration ((object) Foo.i);
		if (Bar.j != 5)
			throw new Exception ("Wrong bar val");

		return 0;
        }
}
