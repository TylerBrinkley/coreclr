// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

////////////////////////////////////////////////////////////////////////////////
// JitHelpers
//    Low-level Jit Helpers
////////////////////////////////////////////////////////////////////////////////

using System.Threading;
using System.Diagnostics;
using Internal.Runtime.CompilerServices;

namespace System.Runtime.CompilerServices
{
    // Wrapper for address of a string variable on stack
    internal struct StringHandleOnStack
    {
        private IntPtr m_ptr;

        internal StringHandleOnStack(IntPtr pString)
        {
            m_ptr = pString;
        }
    }

    // Wrapper for address of a object variable on stack
    internal struct ObjectHandleOnStack
    {
        private IntPtr m_ptr;

        internal ObjectHandleOnStack(IntPtr pObject)
        {
            m_ptr = pObject;
        }
    }

    // Wrapper for StackCrawlMark
    internal struct StackCrawlMarkHandle
    {
        private IntPtr m_ptr;

        internal StackCrawlMarkHandle(IntPtr stackMark)
        {
            m_ptr = stackMark;
        }
    }

    // Helper class to assist with unsafe pinning of arbitrary objects. The typical usage pattern is:
    // fixed (byte * pData = &JitHelpers.GetPinningHelper(value).m_data)
    // {
    //    ... pData is what Object::GetData() returns in VM ...
    // }
    internal class PinningHelper
    {
        public byte m_data;
    }

    internal class ArrayPinningHelper
    {
        public IntPtr m_lengthAndPadding;
        public byte m_arrayData;
    }

    internal static class JitHelpers
    {
        // The special dll name to be used for DllImport of QCalls
        internal const string QCall = "QCall";

        // Wraps object variable into a handle. Used to return managed strings from QCalls.
        // s has to be a local variable on the stack.
        internal static StringHandleOnStack GetStringHandleOnStack(ref string s)
        {
            return new StringHandleOnStack(UnsafeCastToStackPointer(ref s));
        }

        // Wraps object variable into a handle. Used to pass managed object references in and out of QCalls.
        // o has to be a local variable on the stack.
        internal static ObjectHandleOnStack GetObjectHandleOnStack<T>(ref T o) where T : class
        {
            return new ObjectHandleOnStack(UnsafeCastToStackPointer(ref o));
        }

        // Wraps StackCrawlMark into a handle. Used to pass StackCrawlMark to QCalls.
        // stackMark has to be a local variable on the stack.
        internal static StackCrawlMarkHandle GetStackCrawlMarkHandle(ref StackCrawlMark stackMark)
        {
            return new StackCrawlMarkHandle(UnsafeCastToStackPointer(ref stackMark));
        }

#if DEBUG
        // Internal method for getting a raw pointer for handles in JitHelpers.
        // The reference has to point into a local stack variable in order so it can not be moved by the GC.
        internal static IntPtr UnsafeCastToStackPointer<T>(ref T val)
        {
            IntPtr p = UnsafeCastToStackPointerInternal<T>(ref val);
            Debug.Assert(IsAddressInStack(p), "Pointer not in the stack!");
            return p;
        }

        private static IntPtr UnsafeCastToStackPointerInternal<T>(ref T val)
        {
            // The body of this function will be replaced by the EE with unsafe code that just returns val!!!
            // See getILIntrinsicImplementation for how this happens.  
            throw new InvalidOperationException();
        }
#else // DEBUG

        internal static IntPtr UnsafeCastToStackPointer<T>(ref T val)
        {
            // The body of this function will be replaced by the EE with unsafe code that just returns o!!!
            // See getILIntrinsicImplementation for how this happens.  
            throw new InvalidOperationException();
        }
#endif // DEBUG

        // Set the given element in the array without any type or range checks
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        internal static extern void UnsafeSetArrayElement(object[] target, int index, object element);

        // Used for unsafe pinning of arbitrary objects.
        internal static PinningHelper GetPinningHelper(object o)
        {
            return Unsafe.As<PinningHelper>(o);
        }

#if DEBUG
        [MethodImplAttribute(MethodImplOptions.InternalCall)]
        private static extern bool IsAddressInStack(IntPtr ptr);
#endif

        internal static ref byte GetRawSzArrayData(this Array array)
        {
            // The body of this function will be replaced by the EE with unsafe code!!!
            // See getILIntrinsicImplementation for how this happens.
            typeof(ArrayPinningHelper).ToString(); // Type used by the actual method body
            throw new InvalidOperationException();
        }
    }
}
