// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Collections.Generic;

namespace System.Flags
{
    public static class FlagEnum
    {
        public static TEnum AllFlags<TEnum>() where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.AllFlags;

        public static TEnum CombineFlags<TEnum>(this TEnum value, TEnum flags) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.CombineFlags(value, flags);

        public static TEnum CombineFlags<TEnum>(TEnum flag0, TEnum flag1, TEnum flag2) where TEnum : struct, Enum => flag0.CombineFlags(flag1).CombineFlags(flag2);

        public static TEnum CombineFlags<TEnum>(TEnum flag0, TEnum flag1, TEnum flag2, TEnum flag3) where TEnum : struct, Enum => flag0.CombineFlags(flag1).CombineFlags(flag2).CombineFlags(flag3);

        public static TEnum CombineFlags<TEnum>(TEnum flag0, TEnum flag1, TEnum flag2, TEnum flag3, TEnum flag4) where TEnum : struct, Enum => flag0.CombineFlags(flag1).CombineFlags(flag2).CombineFlags(flag3).CombineFlags(flag4);

        public static TEnum CombineFlags<TEnum>(params TEnum[] flags) where TEnum : struct, Enum => CombineFlags((IEnumerable<TEnum>)flags);

        public static TEnum CombineFlags<TEnum>(IEnumerable<TEnum> flags) where TEnum : struct, Enum
        {
            if (flags == null)
            {
                throw new ArgumentNullException(nameof(flags));
            }

            return Enum.EnumBridge<TEnum>.Bridge.CombineFlags(flags);
        }

        public static TEnum CommonFlags<TEnum>(this TEnum value, TEnum flags) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.CommonFlags(value, flags);

        public static IEnumerable<TEnum> GetFlags<TEnum>(this TEnum value) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.GetFlags(value);

        public static bool HasAllFlags<TEnum>(this TEnum value) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.HasAllFlags(value);

        public static bool HasAllFlags<TEnum>(this TEnum value, TEnum flags) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.HasAllFlags(value, flags);

        public static bool HasAnyFlags<TEnum>(this TEnum value) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.HasAnyFlags(value);

        public static bool HasAnyFlags<TEnum>(this TEnum value, TEnum flags) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.HasAnyFlags(value, flags);

        public static bool IsFlagEnum<TEnum>() where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.IsFlagEnum;

        public static bool IsValidFlagCombination<TEnum>(this TEnum value) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.IsValidFlagCombination(value);

        public static TEnum RemoveFlags<TEnum>(this TEnum value, TEnum flags) where TEnum : struct, Enum => Enum.EnumBridge<TEnum>.Bridge.RemoveFlags(value, flags);

        public static object AllFlags(Type enumType) => Enum.GetBridge(enumType).AllFlags;

        public static object CombineFlags(Type enumType, object value, object flags)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }
            if (flags == null)
            {
                throw new ArgumentNullException(nameof(flags));
            }

            return Enum.GetBridge(enumType).CombineFlags(value, flags);
        }

        public static object CombineFlags(Type enumType, params object[] flags) => CombineFlags(enumType, (IEnumerable<object>)flags);

        public static object CombineFlags(Type enumType, IEnumerable<object> flags)
        {
            if (flags == null)
            {
                throw new ArgumentNullException(nameof(flags));
            }

            return Enum.GetBridge(enumType).CombineFlags(flags);
        }

        public static object CommonFlags(Type enumType, object value, object flags)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }
            if (flags == null)
            {
                throw new ArgumentNullException(nameof(flags));
            }

            return Enum.GetBridge(enumType).CommonFlags(value, flags);
        }

        public static IEnumerable<object> GetFlags(Type enumType, object value)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }

            foreach (object flag in Enum.GetBridge(enumType).GetFlags(value))
            {
                yield return flag;
            }
        }

        public static bool HasAllFlags(Type enumType, object value)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }

            return Enum.GetBridge(enumType).HasAllFlags(value);
        }

        public static bool HasAllFlags(Type enumType, object value, object flags)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }
            if (flags == null)
            {
                throw new ArgumentNullException(nameof(flags));
            }

            return Enum.GetBridge(enumType).HasAllFlags(value, flags);
        }

        public static bool HasAnyFlags(Type enumType, object value)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }

            return Enum.GetBridge(enumType).HasAnyFlags(value);
        }

        public static bool HasAnyFlags(Type enumType, object value, object flags)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }
            if (flags == null)
            {
                throw new ArgumentNullException(nameof(flags));
            }

            return Enum.GetBridge(enumType).HasAnyFlags(value, flags);
        }

        public static bool IsFlagEnum(Type enumType) => Enum.GetBridge(enumType).IsFlagEnum;

        public static bool IsValidFlagCombination(Type enumType, object value)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }

            return Enum.GetBridge(enumType).IsValidFlagCombination(value);
        }

        public static object RemoveFlags(Type enumType, object value, object flags)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }
            if (flags == null)
            {
                throw new ArgumentNullException(nameof(flags));
            }

            return Enum.GetBridge(enumType).RemoveFlags(value, flags);
        }
    }
}
