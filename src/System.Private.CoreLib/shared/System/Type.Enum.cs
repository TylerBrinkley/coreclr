// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

namespace System
{
    //
    // This file collects a set of Enum-related apis that run when the Type is subclassed by an application.
    // None of it runs on normal Type objects supplied by the runtime (as those types override these methods.)
    //
    // Since app-subclassed Types are "untrusted classes" that may or may not implement the complete surface area correctly,
    // this code should be considered brittle and not changed lightly.
    //
    public abstract partial class Type
    {
        public virtual bool IsEnumDefined(object value)
        {
            return Enum.IsDefined(this, value);
        }

        public virtual string GetEnumName(object value)
        {
            return Enum.GetName(this, value);
        }

        public virtual string[] GetEnumNames()
        {
            return Enum.GetNames(this);
        }
    }
}
