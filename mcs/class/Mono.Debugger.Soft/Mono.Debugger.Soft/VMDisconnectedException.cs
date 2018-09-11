using System;

namespace Mono.Debugger.Soft
{
	public class VMDisconnectedException : Exception {
		
		public VMDisconnectedException () : base () {
			Console.WriteLine (Environment.StackTrace);
		}
	}

	public class VMCrashException : VMDisconnectedException {
		public readonly string Dump;
		public readonly ulong Hash;

		public VMCrashException (string dump, ulong hash) : base () {
			this.Dump = dump;
			this.Hash = hash;
		}
	}
}
