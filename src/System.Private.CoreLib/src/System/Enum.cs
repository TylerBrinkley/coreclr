// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Reflection;
using System.Text;
using System.Collections;
using System.Globalization;
using System.Runtime.CompilerServices;
using Internal.Runtime.CompilerServices;
using System.Collections.Generic;
using shared.System.ComponentModel;
using System.ComponentModel;
using System.Diagnostics;

// The code below includes partial support for float/double and
// pointer sized enums.
//
// The type loader does not prohibit such enums, and older versions of
// the ECMA spec include them as possible enum types.
//
// However there are many things broken throughout the stack for
// float/double/intptr/uintptr enums. There was a conscious decision
// made to not fix the whole stack to work well for them because of
// the right behavior is often unclear, and it is hard to test and
// very low value because of such enums cannot be expressed in C#.

namespace System
{
    [Serializable]
    [System.Runtime.CompilerServices.TypeForwardedFrom("mscorlib, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089")]
    public abstract class Enum : ValueType, IComparable, IFormattable, IConvertible
    {
        #region Private Constants
        private const char enumSeparatorChar = ',';
        private const string enumSeparatorString = ", ";
        #endregion

        #region Private Static Methods
        internal static IEnumBridge GetBridge(Type enumType)
        {
            if (enumType == null)
            {
                throw new ArgumentNullException(nameof(enumType));
            }
            
            if (!(enumType is RuntimeType rtType))
            {
                throw new ArgumentException(SR.Arg_MustBeType, nameof(enumType));
            }

            if (!enumType.IsEnum)
            {
                throw new ArgumentException(SR.Arg_MustBeEnum, nameof(enumType));
            }

            return GetBridge(rtType);
        }

        internal static IEnumBridge GetBridge(RuntimeType rtType)
        {
            if (!(rtType.GenericCache is IEnumBridge bridge))
            {
                bridge = (IEnumBridge)typeof(EnumBridge<>).MakeGenericType(rtType).GetField(nameof(EnumBridge<DayOfWeek>.Bridge), BindingFlags.Static | BindingFlags.Public).GetValue(null);
                rtType.GenericCache = bridge;
            }

            return bridge;
        }

        private static Type GetUnderlyingTypeInternal(Type enumType)
        {
            if (!enumType.IsEnum)
            {
                throw new ArgumentException(SR.Arg_MustBeEnum, nameof(enumType));
            }

            FieldInfo[] fields = enumType.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
            if (fields == null || fields.Length != 1)
            {
                throw new ArgumentException(SR.Argument_InvalidEnum, nameof(enumType));
            }

            return fields[0].FieldType;
        }
        #endregion

        #region Public Static Methods
        public static bool TryParse(Type enumType, string value, out object result) => TryParse(enumType, value, false, out result);

        public static bool TryParse(Type enumType, string value, bool ignoreCase, out object result) => TryParse(enumType, value.AsSpan(), ignoreCase, out result);

        public static bool TryParse(Type enumType, ReadOnlySpan<char> value, out object result) => TryParse(enumType, value, false, out result);

        public static bool TryParse(Type enumType, ReadOnlySpan<char> value, bool ignoreCase, out object result) => GetBridge(enumType).TryParse(value, ignoreCase, out result);

        public static bool TryParse<TEnum>(string value, out TEnum result) where TEnum : struct => TryParse(value, false, out result);

        public static bool TryParse<TEnum>(string value, bool ignoreCase, out TEnum result) where TEnum : struct
        {
            IEnumBridge<TEnum> bridge = EnumBridge<TEnum>.Bridge;
            if (bridge == null)
            {
                throw new ArgumentException(SR.Arg_MustBeEnum, "enumType");
            }
            return bridge.TryParse(value.AsSpan(), ignoreCase, out result);
        }

        public static bool TryParse<TEnum>(ReadOnlySpan<char> value, out TEnum result) where TEnum : struct, Enum => TryParse(value, false, out result);

        public static bool TryParse<TEnum>(ReadOnlySpan<char> value, bool ignoreCase, out TEnum result) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.TryParse(value, ignoreCase, out result);

        public static object Parse(Type enumType, string value) => Parse(enumType, value, false);

        public static object Parse(Type enumType, string value, bool ignoreCase)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }

            return Parse(enumType, value.AsSpan(), ignoreCase);
        }

        public static object Parse(Type enumType, ReadOnlySpan<char> value) => Parse(enumType, value, false);

        public static object Parse(Type enumType, ReadOnlySpan<char> value, bool ignoreCase) => GetBridge(enumType).Parse(value, ignoreCase);

        public static TEnum Parse<TEnum>(string value) where TEnum : struct => Parse<TEnum>(value, false);

        public static TEnum Parse<TEnum>(string value, bool ignoreCase) where TEnum : struct
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }

            IEnumBridge<TEnum> bridge = EnumBridge<TEnum>.Bridge;
            if (bridge == null)
            {
                throw new ArgumentException(SR.Arg_MustBeEnum, "enumType");
            }
            return bridge.Parse(value.AsSpan(), ignoreCase);
        }

        public static TEnum Parse<TEnum>(ReadOnlySpan<char> value) where TEnum : struct, Enum => Parse<TEnum>(value, false);

        public static TEnum Parse<TEnum>(ReadOnlySpan<char> value, bool ignoreCase) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.Parse(value, ignoreCase);

        public static Type GetUnderlyingType(Type enumType) => GetBridge(enumType).UnderlyingType;

        public static Type GetUnderlyingType<TEnum>() where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.UnderlyingType;

        public static Array GetValues(Type enumType) => GetBridge(enumType).GetValues();

        public static IEnumerable<TEnum> GetValues<TEnum>() where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.GetValues();

        public static string GetName(Type enumType, object value) => GetBridge(enumType).GetName(value);

        public static string GetName<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.GetName(value);

        public static string[] GetNames(Type enumType)
        {
            IEnumBridge bridge = GetBridge(enumType);

            string[] names = new string[bridge.Count];
            int i = 0;
            foreach (string name in bridge.GetNames())
            {
                names[i] = name;
                ++i;
            }

            return names;
        }

        public static IEnumerable<string> GetNames<TEnum>() where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.GetNames();

        public static object ToObject(Type enumType, object value) => GetBridge(enumType).ToObject(value);

        public static TEnum ToObject<TEnum>(object value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);

        public static bool IsDefined(Type enumType, object value) => GetBridge(enumType).IsDefined(value);

        public static bool IsDefined<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.IsDefined(value);

        public static string Format(Type enumType, object value, string format)
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }

            return GetBridge(enumType).Format(value, format);
        }

        public static string Format<TEnum>(TEnum value, string format) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.Format(value, format);
        #endregion

        #region Definitions
        #region EnumBridge
        internal static class EnumBridge<TEnum>
        {
            public readonly static IEnumBridge<TEnum> Bridge = (IEnumBridge<TEnum>)CreateEnumBridge(typeof(TEnum));
        }

        private static IEnumBridge CreateEnumBridge(Type enumType)
        {
            Type underlyingType = GetUnderlyingTypeInternal(enumType);
            Type operatorsType = GetOperatorsType(underlyingType);
            return operatorsType != null ? (IEnumBridge)Activator.CreateInstance(typeof(EnumBridge<,,>).MakeGenericType(enumType, underlyingType, operatorsType)) : null;
        }

        private static Type GetOperatorsType(Type underlyingType)
        {
            switch (Type.GetTypeCode(underlyingType))
            {
                case TypeCode.SByte:
                    return typeof(SByteOperators);
                case TypeCode.Byte:
                    return typeof(ByteOperators);
                case TypeCode.Int16:
                    return typeof(Int16Operators);
                case TypeCode.UInt16:
                    return typeof(UInt16Operators);
                case TypeCode.Int32:
                    return typeof(Int32Operators);
                case TypeCode.UInt32:
                    return typeof(UInt32Operators);
                case TypeCode.Int64:
                    return typeof(Int64Operators);
                case TypeCode.UInt64:
                    return typeof(UInt64Operators);
                case TypeCode.Boolean:
                    return typeof(BooleanOperators);
                case TypeCode.Char:
                    return typeof(CharOperators);
                case TypeCode.Single:
                    return typeof(SingleOperators);
                case TypeCode.Double:
                    return typeof(DoubleOperators);
                default:
                    if (underlyingType == typeof(IntPtr))
                    {
                        return typeof(IntPtrOperators);
                    }
                    if (underlyingType == typeof(UIntPtr))
                    {
                        return typeof(UIntPtrOperators);
                    }
                    return null;
            }
        }

        internal interface IEnumBridgeCommon
        {
            bool IsFlagEnum { get; }
            Type UnderlyingType { get; }
            int Count { get; }
            TypeCode TypeCode { get; }

            IEnumerable<string> GetNames();
        }

        internal interface IEnumBridge : IEnumBridgeCommon
        {
            object AllFlags { get; }

            object CombineFlags(IEnumerable<object> flags);
            object CombineFlags(object value, object flags);
            object CommonFlags(object value, object flags);
            int CompareTo(Enum value, object other);
            bool Equals(Enum value, object other);
            string Format(object value, string format);
            IEnumerable GetFlags(object value);
            int GetHashCode(Enum value);
            string GetName(object value);
            Array GetValues();
            object GetUnderlyingValue(Enum value);
            bool HasAllFlags(object value);
            bool HasAllFlags(object value, object flags);
            bool HasAnyFlags(object value);
            bool HasAnyFlags(object value, object flags);
            bool IsDefined(object value);
            bool IsValidFlagCombination(object value);
            object Parse(ReadOnlySpan<char> value, bool ignoreCase);
            object RemoveFlags(object value, object flags);
            bool ToBoolean(Enum value);
            byte ToByte(Enum value);
            char ToChar(Enum value);
            decimal ToDecimal(Enum value);
            double ToDouble(Enum value);
            short ToInt16(Enum value);
            int ToInt32(Enum value);
            long ToInt64(Enum value);
            object ToObject(object value);
            object ToObject(ulong value);
            sbyte ToSByte(Enum value);
            float ToSingle(Enum value);
            string ToString(Enum value);
            string ToString(Enum value, string format);
            ushort ToUInt16(Enum value);
            uint ToUInt32(Enum value);
            ulong ToUInt64(Enum value);
            bool TryParse(ReadOnlySpan<char> value, bool ignoreCase, out object result);
        }

        internal interface IEnumBridge<TEnum> : IEnumBridgeCommon
        {
            TEnum AllFlags { get; }

            TEnum CombineFlags(IEnumerable<TEnum> flags);
            TEnum CombineFlags(TEnum value, TEnum flags);
            TEnum CommonFlags(TEnum value, TEnum flags);
            int CompareTo(TEnum value, TEnum other);
            bool Equals(TEnum value, TEnum other);
            string Format(TEnum value, string format);
            IEnumerable<TEnum> GetFlags(TEnum value);
            int GetHashCode(TEnum value);
            string GetName(TEnum value);
            IEnumerable<TEnum> GetValues();
            bool HasAllFlags(TEnum value);
            bool HasAllFlags(TEnum value, TEnum flags);
            bool HasAnyFlags(TEnum value);
            bool HasAnyFlags(TEnum value, TEnum flags);
            bool IsDefined(TEnum value);
            bool IsValidFlagCombination(TEnum value);
            TEnum Parse(ReadOnlySpan<char> value, bool ignoreCase);
            TEnum RemoveFlags(TEnum value, TEnum flags);
            byte ToByte(TEnum value);
            short ToInt16(TEnum value);
            int ToInt32(TEnum value);
            long ToInt64(TEnum value);
            TEnum ToObject(object value);
            TEnum ToObject(ulong value);
            sbyte ToSByte(TEnum value);
            string ToString(TEnum value);
            string ToString(TEnum value, string format);
            ushort ToUInt16(TEnum value);
            uint ToUInt32(TEnum value);
            ulong ToUInt64(TEnum value);
            bool TryParse(ReadOnlySpan<char> value, bool ignoreCase, out TEnum result);
        }

        private sealed class EnumBridge<TEnum, TUnderlying, TUnderlyingOperators> : IEnumBridge<TEnum>, IEnumBridge
            where TEnum : struct, Enum
            where TUnderlying : struct, IEquatable<TUnderlying>
            where TUnderlyingOperators : struct, IUnderlyingOperators<TUnderlying>
        {
            private static readonly TUnderlyingOperators s_operators = new TUnderlyingOperators();

            private static readonly EnumCache<TUnderlying, TUnderlyingOperators> s_cache = new EnumCache<TUnderlying, TUnderlyingOperators>(typeof(TEnum));

            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            private static TUnderlying ToUnderlying(TEnum value) => Unsafe.As<TEnum, TUnderlying>(ref value);

            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            private static TEnum ToEnum(TUnderlying value) => Unsafe.As<TUnderlying, TEnum>(ref value);

            public TEnum Parse(ReadOnlySpan<char> value, bool ignoreCase) => ToEnum(s_cache.Parse(value, ignoreCase));

            public bool TryParse(ReadOnlySpan<char> value, bool ignoreCase, out TEnum result)
            {
                bool success = s_cache.TryParse(value, ignoreCase, out TUnderlying underlying);
                result = ToEnum(underlying);
                return success;
            }

            public int CompareTo(TEnum value, TEnum other) => s_operators.CompareTo(ToUnderlying(value), ToUnderlying(other));

            public bool Equals(TEnum value, TEnum other) => ToUnderlying(value).Equals(ToUnderlying(other));

            public int GetHashCode(TEnum value) => ToUnderlying(value).GetHashCode();

            public string Format(TEnum value, string format) => s_cache.Format(ToUnderlying(value), format);

            public string GetName(TEnum value) => s_cache.GetName(ToUnderlying(value));

            public IEnumerable<string> GetNames() => s_cache.GetNames();

            public Type UnderlyingType { get; } = typeof(TUnderlying);

            public TypeCode TypeCode { get; } = Type.GetTypeCode(typeof(TUnderlying));

            public IEnumerable<TEnum> GetValues()
            {
                foreach (KeyValuePair<TUnderlying, List<EnumCache<TUnderlying, TUnderlyingOperators>.EnumMemberInternal>> pair in s_cache._valueMap)
                {
                    TEnum value = ToEnum(pair.Key);
                    for (int i = 0; i < pair.Value.Count; ++i)
                    {
                        yield return value;
                    }
                }
            }

            public bool IsDefined(TEnum value) => s_cache.IsDefined(ToUnderlying(value));

            public byte ToByte(TEnum value) => s_operators.ToByte(ToUnderlying(value));

            public short ToInt16(TEnum value) => s_operators.ToInt16(ToUnderlying(value));

            public int ToInt32(TEnum value) => s_operators.ToInt32(ToUnderlying(value));

            public long ToInt64(TEnum value) => s_operators.ToInt64(ToUnderlying(value));

            public TEnum ToObject(object value) => ToEnum(s_cache.ToObject(value));

            public TEnum ToObject(ulong value) => ToEnum(s_operators.ToObject(value));

            public sbyte ToSByte(TEnum value) => s_operators.ToSByte(ToUnderlying(value));

            public string ToString(TEnum value) => s_cache.ToString(ToUnderlying(value));

            public string ToString(TEnum value, string format) => s_cache.ToString(ToUnderlying(value), format);

            public ushort ToUInt16(TEnum value) => s_operators.ToUInt16(ToUnderlying(value));

            public uint ToUInt32(TEnum value) => s_operators.ToUInt32(ToUnderlying(value));

            public ulong ToUInt64(TEnum value) => s_operators.ToUInt64(ToUnderlying(value));

            public TEnum AllFlags => ToEnum(s_cache._allFlags);

            public TEnum CombineFlags(TEnum value, TEnum flags) => ToEnum(s_operators.Or(ToUnderlying(value), ToUnderlying(flags)));

            public TEnum CombineFlags(IEnumerable<TEnum> flags)
            {
                Debug.Assert(flags != null);

                TUnderlying result = s_operators.Zero;
                foreach (TEnum flag in flags)
                {
                    result = s_operators.Or(result, ToUnderlying(flag));
                }
                return ToEnum(result);
            }

            public TEnum CommonFlags(TEnum value, TEnum flags) => ToEnum(s_operators.And(ToUnderlying(value), ToUnderlying(flags)));

            public IEnumerable<TEnum> GetFlags(TEnum value)
            {
                TUnderlying validValue = s_operators.And(ToUnderlying(value), s_cache._allFlags);
                bool isLessThanZero = s_operators.LessThan(validValue, s_operators.Zero);
                for (TUnderlying currentValue = s_operators.One; (isLessThanZero || !s_operators.LessThan(validValue, currentValue)) && !currentValue.Equals(s_operators.Zero); currentValue = s_operators.LeftShift(currentValue, 1))
                {
                    if (!s_operators.Or(validValue, currentValue).Equals(s_operators.Zero))
                    {
                        yield return ToEnum(currentValue);
                    }
                }
            }

            public bool HasAllFlags(TEnum value) => ToUnderlying(value).Equals(s_cache._allFlags);

            public bool HasAllFlags(TEnum value, TEnum flags) => s_operators.And(ToUnderlying(value), ToUnderlying(flags)).Equals(ToUnderlying(flags));

            public bool HasAnyFlags(TEnum value) => !ToUnderlying(value).Equals(s_operators.Zero);

            public bool HasAnyFlags(TEnum value, TEnum flags) => !s_operators.And(ToUnderlying(value), ToUnderlying(flags)).Equals(s_operators.Zero);

            public bool IsFlagEnum => s_cache._isFlagEnum;

            public bool IsValidFlagCombination(TEnum value) => s_operators.And(ToUnderlying(value), s_cache._allFlags).Equals(ToUnderlying(value));

            public TEnum RemoveFlags(TEnum value, TEnum flags) => ToEnum(s_operators.And(ToUnderlying(value), s_operators.Not(ToUnderlying(flags))));

            public int Count => s_cache.Count;

            #region IEnumBridge
            private static TEnum ToEnum(object value)
            {
                Debug.Assert(value != null);
                
                if (!(value is TEnum enumValue))
                {
                    throw new ArgumentException(SR.Format(SR.Arg_EnumAndObjectMustBeSameType, value.GetType().ToString(), typeof(TEnum).ToString()));
                }
                return enumValue;
            }

            object IEnumBridge.AllFlags => AllFlags;

            public object CombineFlags(IEnumerable<object> flags)
            {
                Debug.Assert(flags != null);

                TEnum result = default;
                foreach (object flag in flags)
                {
                    if (flag == null)
                    {
                        throw new ArgumentNullException(nameof(flag));
                    }

                    result = CombineFlags(result, ToEnum(flag));
                }
                return result;
            }

            public object CombineFlags(object value, object flags) => CombineFlags(ToEnum(value), ToEnum(flags));

            public object CommonFlags(object value, object flags) => CommonFlags(ToEnum(value), ToEnum(flags));

            public int CompareTo(Enum value, object other)
            {
                Debug.Assert(other != null);

                return CompareTo((TEnum)value, ToEnum(other));
            }

            public bool Equals(Enum value, object other) => other is TEnum enumValue ? Equals((TEnum)value, enumValue) : false;

            public string Format(object value, string format) => Format(value is TUnderlying underlyingValue ? ToEnum(underlyingValue) : ToEnum(value), format);

            public IEnumerable GetFlags(object value) => GetFlags(ToEnum(value));

            public int GetHashCode(Enum value) => ToUnderlying((TEnum)value).GetHashCode();

            public string GetName(object value) => value is TEnum enumValue ? GetName(enumValue) : s_cache.GetName(value);

            public object GetUnderlyingValue(Enum value) => ToUnderlying((TEnum)value);

            Array IEnumBridge.GetValues()
            {
                TEnum[] array = new TEnum[Count];
                int i = 0;
                foreach (TEnum value in GetValues())
                {
                    array[i] = value;
                    ++i;
                }
                return array;
            }

            public bool HasAllFlags(object value) => HasAllFlags(ToEnum(value));

            public bool HasAllFlags(object value, object flags) => HasAllFlags(ToEnum(value), ToEnum(flags));

            public bool HasAnyFlags(object value) => HasAnyFlags(ToEnum(value));

            public bool HasAnyFlags(object value, object flags) => HasAnyFlags(ToEnum(value), ToEnum(flags));

            public bool IsDefined(object value) => value is TEnum enumValue ? IsDefined(enumValue) : s_cache.IsDefined(value);

            public bool IsValidFlagCombination(object value) => IsValidFlagCombination(ToEnum(value));

            object IEnumBridge.Parse(ReadOnlySpan<char> value, bool ignoreCase) => Parse(value, ignoreCase);

            public object RemoveFlags(object value, object flags) => RemoveFlags(ToEnum(value), ToEnum(flags));

            public bool ToBoolean(Enum value) => s_operators.ToBoolean(ToUnderlying((TEnum)value));

            public byte ToByte(Enum value) => ToByte((TEnum)value);

            public char ToChar(Enum value) => s_operators.ToChar(ToUnderlying((TEnum)value));

            public decimal ToDecimal(Enum value) => s_operators.ToDecimal(ToUnderlying((TEnum)value));

            public double ToDouble(Enum value) => s_operators.ToDouble(ToUnderlying((TEnum)value));

            public short ToInt16(Enum value) => ToInt16((TEnum)value);

            public int ToInt32(Enum value) => ToInt32((TEnum)value);

            public long ToInt64(Enum value) => ToInt64((TEnum)value);

            object IEnumBridge.ToObject(object value) => ToObject(value);

            object IEnumBridge.ToObject(ulong value) => ToObject(value);

            public sbyte ToSByte(Enum value) => ToSByte((TEnum)value);

            public float ToSingle(Enum value) => s_operators.ToSingle(ToUnderlying((TEnum)value));

            public string ToString(Enum value) => ToString((TEnum)value);

            public string ToString(Enum value, string format) => ToString((TEnum)value, format);

            public ushort ToUInt16(Enum value) => ToUInt16((TEnum)value);

            public uint ToUInt32(Enum value) => ToUInt32((TEnum)value);

            public ulong ToUInt64(Enum value) => ToUInt64((TEnum)value);

            public bool TryParse(ReadOnlySpan<char> value, bool ignoreCase, out object result)
            {
                bool success = TryParse(value, ignoreCase, out TEnum enumResult);
                result = success ? (object)enumResult : null;
                return success;
            }
            #endregion
        }
        #endregion

        #region EnumCache
        private sealed class EnumCache<TUnderlying, TUnderlyingOperators>
            where TUnderlying : struct, IEquatable<TUnderlying>
            where TUnderlyingOperators : struct, IUnderlyingOperators<TUnderlying>
        {
            internal static readonly TUnderlyingOperators s_operators = new TUnderlyingOperators();

            private readonly Type _enumType;

            internal readonly TUnderlying _allFlags;

            internal readonly bool _isFlagEnum;

            private readonly bool _isContiguous;

            private readonly TUnderlying _maxDefined;

            private readonly TUnderlying _minDefined;

            internal readonly Dictionary<TUnderlying, List<EnumMemberInternal>> _valueMap;

            private Dictionary<int, List<EnumMemberInternal>> _nameMap;

            private Dictionary<int, List<EnumMemberInternal>> NameMap
            {
                get
                {
                    Dictionary<int, List<EnumMemberInternal>> nameMap = _nameMap;
                    if (nameMap == null)
                    {
                        nameMap = new Dictionary<int, List<EnumMemberInternal>>(Count);
                        foreach (KeyValuePair<TUnderlying, List<EnumMemberInternal>> pair in _valueMap)
                        {
                            AddToNameMap(pair.Value[0]);
                        }
                        if (Count != _valueMap.Count)
                        {
                            foreach (KeyValuePair<TUnderlying, List<EnumMemberInternal>> pair in _valueMap)
                            {
                                for (int i = 1; i < pair.Value.Count; ++i)
                                {
                                    AddToNameMap(pair.Value[i]);
                                }
                            }
                        }

                        nameMap.TrimExcess();
                        foreach (KeyValuePair<int, List<EnumMemberInternal>> pair in nameMap)
                        {
                            pair.Value.TrimExcess();
                        }

                        _nameMap = nameMap;
                    }
                    return nameMap;

                    void AddToNameMap(EnumMemberInternal member)
                    {
                        int hash = CompareInfo.GetIgnoreCaseHash(member.Name);
                        if (!nameMap.TryGetValue(hash, out List<EnumMemberInternal> list))
                        {
                            list = new List<EnumMemberInternal>();
                            nameMap.Add(hash, list);
                        }
                        list.Add(member);
                    }
                }
            }

            public int Count { get; }

            public EnumCache(Type enumType)
            {
                _enumType = enumType;
                _isFlagEnum = enumType.IsDefined(typeof(FlagsAttribute), false);

                FieldInfo[] fields = enumType.GetFields(BindingFlags.Public | BindingFlags.Static);
                _valueMap = new Dictionary<TUnderlying, List<EnumMemberInternal>>(fields.Length);
                if (fields.Length == 0)
                {
                    return;
                }
                Count = fields.Length;

                foreach (FieldInfo field in fields)
                {
                    TUnderlying value = (TUnderlying)field.GetRawConstantValue();
                    EnumMemberInternal member = new EnumMemberInternal(value, field.Name);
                    if (!_valueMap.TryGetValue(value, out List<EnumMemberInternal> list))
                    {
                        list = new List<EnumMemberInternal> { member };
                        _valueMap.Add(value, list);
                        // Is Power of Two
                        if (s_operators.And(value, s_operators.Subtract(value, s_operators.One)).Equals(s_operators.Zero))
                        {
                            _allFlags = s_operators.Or(_allFlags, value);
                        }
                    }
                    else if (field.GetCustomAttribute<PrimaryAttribute>(false) != null)
                    {
                        list.Insert(0, member);
                    }
                    else
                    {
                        list.Add(member);
                    }
                }

                bool isInOrder = true;
                EnumMemberInternal previous = default;
                bool isFirst = true;
                foreach (KeyValuePair<TUnderlying, List<EnumMemberInternal>> pair in _valueMap)
                {
                    EnumMemberInternal current = pair.Value[0];
                    if (isFirst)
                    {
                        _minDefined = current.Value;
                        isFirst = false;
                    }
                    else if (previous.CompareTo(current) > 0)
                    {
                        isInOrder = false;
                        break;
                    }
                    previous = current;
                }
                if (isInOrder)
                {
                    _maxDefined = previous.Value;
                }
                else
                {
                    // Makes sure is in increasing value order, due to no removals
                    List<KeyValuePair<TUnderlying, List<EnumMemberInternal>>> values = new List<KeyValuePair<TUnderlying, List<EnumMemberInternal>>>(_valueMap);
                    values.Sort((x, y) => x.Value[0].CompareTo(y.Value[0]));
                    _valueMap = new Dictionary<TUnderlying, List<EnumMemberInternal>>(_valueMap.Count);

                    foreach (KeyValuePair<TUnderlying, List<EnumMemberInternal>> pair in values)
                    {
                        _valueMap.Add(pair.Key, pair.Value);
                    }

                    _maxDefined = values[values.Count - 1].Key;
                    _minDefined = values[0].Key;
                }

                _isContiguous = s_operators.Subtract(_maxDefined, s_operators.ToObject((ulong)(_valueMap.Count - 1))).Equals(_minDefined);

                _valueMap.TrimExcess();
                foreach (KeyValuePair<TUnderlying, List<EnumMemberInternal>> pair in _valueMap)
                {
                    pair.Value.TrimExcess();
                }
            }

            public IEnumerable<string> GetNames()
            {
                foreach (KeyValuePair<TUnderlying, List<EnumMemberInternal>> pair in _valueMap)
                {
                    foreach (EnumMemberInternal member in pair.Value)
                    {
                        yield return member.Name;
                    }
                }
            }

            public TUnderlying ToObject(object value)
            {
                if (value == null)
                {
                    throw new ArgumentNullException(nameof(value));
                }

                return s_operators.ToObject(ToUInt64Unchecked(value));
            }

            private static ulong ToUInt64Unchecked(object value)
            {
                // Helper function to silently convert the value to UInt64 from the other base types for enum without throwing an exception.
                // This is need since the Convert functions do overflow checks.
                TypeCode typeCode = Convert.GetTypeCode(value);

                ulong result;
                switch (typeCode)
                {
                    case TypeCode.SByte:
                        result = (ulong)(sbyte)value;
                        break;
                    case TypeCode.Byte:
                        result = (byte)value;
                        break;
                    case TypeCode.Boolean:
                        // direct cast from bool to byte is not allowed
                        result = Convert.ToByte((bool)value);
                        break;
                    case TypeCode.Int16:
                        result = (ulong)(short)value;
                        break;
                    case TypeCode.UInt16:
                        result = (ushort)value;
                        break;
                    case TypeCode.Char:
                        result = (char)value;
                        break;
                    case TypeCode.UInt32:
                        result = (uint)value;
                        break;
                    case TypeCode.Int32:
                        result = (ulong)(int)value;
                        break;
                    case TypeCode.UInt64:
                        result = (ulong)value;
                        break;
                    case TypeCode.Int64:
                        result = (ulong)(long)value;
                        break;
                    // All unsigned types will be directly cast
                    default:
                        throw new ArgumentException(SR.Arg_MustBeEnumBaseTypeOrEnum, nameof(value));
                }

                return result;
            }

            public string GetName(TUnderlying value) => _valueMap.TryGetValue(value, out List<EnumMemberInternal> list) ? list[0].Name : null;

            public string GetName(object value)
            {
                Debug.Assert(value?.GetType() != _enumType);
                if (value == null)
                {
                    throw new ArgumentNullException(nameof(value));
                }

                ulong uint64Value = ToUInt64Unchecked(value);
                return s_operators.IsInValueRange(uint64Value) ? GetName(s_operators.ToObject(uint64Value)) : null;
            }

            public bool IsDefined(TUnderlying value) => _isContiguous ? !(s_operators.LessThan(value, _minDefined) || s_operators.LessThan(_maxDefined, value)) : _valueMap.ContainsKey(value);

            public bool IsDefined(object value)
            {
                Debug.Assert(value?.GetType() != _enumType);

                switch (value)
                {
                    case TUnderlying underlyingValue:
                        return IsDefined(underlyingValue);
                    case string str:
                        if (NameMap.TryGetValue(CompareInfo.GetIgnoreCaseHash(str), out List<EnumMemberInternal> list))
                        {
                            foreach (EnumMemberInternal member in list)
                            {
                                if (str.Equals(member.Name, StringComparison.Ordinal))
                                {
                                    return true;
                                }
                            }
                        }
                        return false;
                    case null:
                        throw new ArgumentNullException(nameof(value));
                    default:
                        Type valueType = value.GetType();

                        // Check if is another type of enum as checking for the current enum type is handled in EnumBridge
                        if (valueType.IsEnum)
                        {
                            throw new ArgumentException(SR.Format(SR.Arg_EnumAndObjectMustBeSameType, valueType.ToString(), _enumType.ToString()));
                        }

                        TypeCode typeCode = Convert.GetTypeCode(value);

                        switch (typeCode)
                        {
                            case TypeCode.SByte:
                            case TypeCode.Byte:
                            case TypeCode.Int16:
                            case TypeCode.UInt16:
                            case TypeCode.Int32:
                            case TypeCode.UInt32:
                            case TypeCode.Int64:
                            case TypeCode.UInt64:
                            case TypeCode.Boolean:
                            case TypeCode.Char:
                                throw new ArgumentException(SR.Format(SR.Arg_EnumUnderlyingTypeAndObjectMustBeSameType, valueType.ToString(), typeof(TUnderlying).ToString()));
                            default:
                                throw new InvalidOperationException(SR.InvalidOperation_UnknownEnumType);
                        }
                }
            }

            public string ToString(TUnderlying value)
            {
                if (_isFlagEnum)
                {
                    return ToStringFlags(value);
                }

                if (_valueMap.TryGetValue(value, out List<EnumMemberInternal> list))
                {
                    return list[0].Name;
                }
                return value.ToString();
            }

            public string ToString(TUnderlying value, string format)
            {
                char formatCh;
                if (format == null || format.Length == 0)
                {
                    formatCh = 'G';
                }
                else if (format.Length != 1)
                {
                    throw new FormatException(SR.Format_InvalidEnumFormatSpecification);
                }
                else
                {
                    formatCh = format[0];
                }

                switch (formatCh)
                {
                    case 'G':
                    case 'g':
                        return ToString(value);
                    case 'D':
                    case 'd':
                        return value.ToString();
                    case 'X':
                    case 'x':
                        return s_operators.ToHexString(value);
                    case 'F':
                    case 'f':
                        return ToStringFlags(value);
                    default:
                        throw new FormatException(SR.Format_InvalidEnumFormatSpecification);
                }
            }

            private string ToStringFlags(TUnderlying value)
            {
                List<EnumMemberInternal> members = new List<EnumMemberInternal>(_valueMap.Count); // Could use an OrderedDictionary to remove allocation
                foreach (KeyValuePair<TUnderlying, List<EnumMemberInternal>> pair in _valueMap)
                {
                    members.Add(pair.Value[0]);
                }

                string returnString;
                if (value.Equals(s_operators.Zero))
                {
                    // For the cases when we have zero
                    if (members.Count > 0 && members[0].Value.Equals(s_operators.Zero))
                    {
                        returnString = members[0].Name; // Zero was one of the enum values.
                    }
                    else
                    {
                        returnString = "0";
                    }
                }
                else
                {
                    StringBuilder sb = StringBuilderCache.Acquire();

                    int index = members.Count - 1;
                    bool firstTime = true;
                    TUnderlying result = value;

                    // We will not optimize this code further to keep it maintainable. There are some boundary checks that can be applied
                    // to minimize the comparsions required. This code works the same for the best/worst case. In general the number of
                    // items in an enum are sufficiently small and not worth the optimization.
                    while (index >= 0)
                    {
                        EnumMemberInternal member = members[index];
                        TUnderlying memberValue = member.Value;
                        if (s_operators.And(result, memberValue).Equals(memberValue) && !memberValue.Equals(s_operators.Zero))
                        {
                            result = s_operators.And(result, s_operators.Not(memberValue));
                            if (!firstTime)
                            {
                                sb.Insert(0, enumSeparatorString);
                            }

                            sb.Insert(0, member.Name);
                            firstTime = false;
                        }

                        index--;
                    }
                    
                    if (!result.Equals(s_operators.Zero))
                    {
                        // We were unable to represent this number as a bitwise or of valid flags
                        returnString = value.ToString();
                    }
                    else
                    {
                        returnString = sb.ToString(); // Return the string representation
                    }

                    StringBuilderCache.Release(sb);
                }
                
                return returnString;
            }

            public string Format(TUnderlying value, string format)
            {
                if (format == null)
                {
                    throw new ArgumentNullException(nameof(format));
                }
                if (format.Length != 1)
                {
                    throw new FormatException(SR.Format_InvalidEnumFormatSpecification);
                }

                return ToString(value, format);
            }

            public TUnderlying Parse(ReadOnlySpan<char> value, bool ignoreCase)
            {
                value = value.Trim();

                if (value.Length == 0)
                {
                    throw new ArgumentException(SR.Arg_MustContainEnumInfo);
                }
                if (TryParseInternal(value, ignoreCase, out TUnderlying result, out bool isNumeric))
                {
                    return result;
                }
                if (isNumeric)
                {
                    throw new OverflowException(s_operators.OverflowMessage);
                }
                throw new ArgumentException(SR.Arg_EnumValueNotFound, nameof(value));
            }

            public bool TryParse(ReadOnlySpan<char> value, bool ignoreCase, out TUnderlying result) => TryParseInternal(value.Trim(), ignoreCase, out result, out _);

            private bool TryParseInternal(ReadOnlySpan<char> value, bool ignoreCase, out TUnderlying result, out bool isNumeric)
            {
                isNumeric = false;
                result = default;
                if (value.Length > 0)
                {
                    char firstNonWhitespaceChar = value[0];
                    if ((char.IsDigit(firstNonWhitespaceChar) || firstNonWhitespaceChar == '-' || firstNonWhitespaceChar == '+'))
                    {
                        isNumeric = true;
                        if (s_operators.TryParse(value, out result))
                        {
                            return true;
                        }
                        result = default;
                    }

                    ReadOnlySpan<char> span = value;
                    
                    while (true)
                    {
                        // Find the next separator, if there is one, otherwise the end of the string.
                        int endIndex = span.IndexOf(enumSeparatorChar);
                        if (endIndex == -1)
                        {
                            endIndex = span.Length;
                        }
                        else if (endIndex + 1 == span.Length)
                        {
                            break;
                        }
                        else
                        {
                            isNumeric = false;
                        }

                        ReadOnlySpan<char> slice = span.Slice(0, endIndex).Trim();

                        if (slice.Length == 0)
                        {
                            break;
                        }

                        EnumMemberInternal foundMember = default;
                        if (NameMap.TryGetValue(CompareInfo.GetIgnoreCaseHash(slice), out List<EnumMemberInternal> list))
                        {
                            foreach (EnumMemberInternal member in list)
                            {
                                if (slice.Equals(member.Name.AsSpan(), StringComparison.Ordinal))
                                {
                                    foundMember = member;
                                    break;
                                }
                            }

                            if (foundMember.Name == null && ignoreCase)
                            {
                                foreach (EnumMemberInternal member in list)
                                {
                                    if (slice.Equals(member.Name.AsSpan(), StringComparison.OrdinalIgnoreCase))
                                    {
                                        foundMember = member;
                                        break;
                                    }
                                }
                            }
                        }

                        if (foundMember.Name == null)
                        {
                            break;
                        }
                        else
                        {
                            result = s_operators.Or(result, foundMember.Value);
                            if (endIndex == span.Length)
                            {
                                isNumeric = false;
                                return true;
                            }
                            else
                            {
                                span = span.Slice(endIndex + 1);
                            }
                        }
                    }
                }

                result = default;
                return false;
            }

            public struct EnumMemberInternal
            {
                public TUnderlying Value;
                public string Name;

                public EnumMemberInternal(TUnderlying value, string name)
                {
                    Value = value;
                    Name = name;
                }

                public int CompareTo(EnumMemberInternal other) => s_operators.ToUInt64Unchecked(Value).CompareTo(s_operators.ToUInt64Unchecked(other.Value));
            }
        }
        #endregion

        #region UnderlyingOperators
        private interface IUnderlyingOperators<TUnderlying>
            where TUnderlying : struct, IEquatable<TUnderlying>
        {
            TUnderlying One { get; }
            TUnderlying Zero { get; }
            string OverflowMessage { get; }
            TUnderlying And(TUnderlying left, TUnderlying right);
            int CompareTo(TUnderlying left, TUnderlying right);
            bool IsInValueRange(ulong value);
            TUnderlying LeftShift(TUnderlying value, int amount);
            bool LessThan(TUnderlying left, TUnderlying right);
            TUnderlying Not(TUnderlying value);
            TUnderlying Or(TUnderlying left, TUnderlying right);
            TUnderlying Subtract(TUnderlying left, TUnderlying right);
            bool ToBoolean(TUnderlying value);
            byte ToByte(TUnderlying value);
            char ToChar(TUnderlying value);
            decimal ToDecimal(TUnderlying value);
            double ToDouble(TUnderlying value);
            string ToHexString(TUnderlying value);
            short ToInt16(TUnderlying value);
            int ToInt32(TUnderlying value);
            long ToInt64(TUnderlying value);
            TUnderlying ToObject(ulong value);
            sbyte ToSByte(TUnderlying value);
            float ToSingle(TUnderlying value);
            ushort ToUInt16(TUnderlying value);
            uint ToUInt32(TUnderlying value);
            ulong ToUInt64(TUnderlying value);
            ulong ToUInt64Unchecked(TUnderlying value);
            bool TryParse(ReadOnlySpan<char> span, out TUnderlying result);
        }

        private struct ByteOperators : IUnderlyingOperators<byte>
        {
            public byte One => 1;

            public byte Zero => 0;

            public string OverflowMessage => SR.Overflow_Byte;

            public byte And(byte left, byte right) => (byte)(left & right);

            public int CompareTo(byte left, byte right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => value <= byte.MaxValue;

            public byte LeftShift(byte value, int amount) => (byte)(value << amount);

            public bool LessThan(byte left, byte right) => left < right;

            public byte Not(byte value) => (byte)~value;

            public byte Or(byte left, byte right) => (byte)(left | right);

            public byte Subtract(byte left, byte right) => (byte)(left - right);

            public bool ToBoolean(byte value) => Convert.ToBoolean(value);

            public byte ToByte(byte value) => value;

            public char ToChar(byte value) => (char)value;

            public decimal ToDecimal(byte value) => value;

            public double ToDouble(byte value) => value;

            public string ToHexString(byte value) => value.ToString("X2");

            public short ToInt16(byte value) => value;

            public int ToInt32(byte value) => value;

            public long ToInt64(byte value) => value;

            public byte ToObject(ulong value) => (byte)value;

            public sbyte ToSByte(byte value) => Convert.ToSByte(value);

            public float ToSingle(byte value) => value;

            public ushort ToUInt16(byte value) => value;

            public uint ToUInt32(byte value) => value;

            public ulong ToUInt64(byte value) => value;

            public ulong ToUInt64Unchecked(byte value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out byte result) => byte.TryParse(span, NumberStyles.Integer, null, out result);
        }

        private struct SByteOperators : IUnderlyingOperators<sbyte>
        {
            public sbyte One => 1;

            public sbyte Zero => 0;

            public string OverflowMessage => SR.Overflow_SByte;

            public sbyte And(sbyte left, sbyte right) => (sbyte)(left & right);

            public int CompareTo(sbyte left, sbyte right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => value <= (ulong)sbyte.MaxValue || value >= unchecked((ulong)sbyte.MinValue);

            public sbyte LeftShift(sbyte value, int amount) => (sbyte)(value << amount);

            public bool LessThan(sbyte left, sbyte right) => left < right;

            public sbyte Not(sbyte value) => (sbyte)~value;

            public sbyte Or(sbyte left, sbyte right) => (sbyte)(left | right);

            public sbyte Subtract(sbyte left, sbyte right) => (sbyte)(left - right);

            public bool ToBoolean(sbyte value) => Convert.ToBoolean(value);

            public byte ToByte(sbyte value) => Convert.ToByte(value);

            public char ToChar(sbyte value) => Convert.ToChar(value);

            public decimal ToDecimal(sbyte value) => value;

            public double ToDouble(sbyte value) => value;

            public string ToHexString(sbyte value) => value.ToString("X2");

            public short ToInt16(sbyte value) => value;

            public int ToInt32(sbyte value) => value;

            public long ToInt64(sbyte value) => value;

            public sbyte ToObject(ulong value) => (sbyte)value;

            public sbyte ToSByte(sbyte value) => value;

            public float ToSingle(sbyte value) => value;

            public ushort ToUInt16(sbyte value) => Convert.ToUInt16(value);

            public uint ToUInt32(sbyte value) => Convert.ToUInt32(value);

            public ulong ToUInt64(sbyte value) => Convert.ToUInt64(value);

            public ulong ToUInt64Unchecked(sbyte value) => (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out sbyte result) => sbyte.TryParse(span, NumberStyles.Integer, null, out result);
        }

        private struct Int16Operators : IUnderlyingOperators<short>
        {
            public short One => 1;

            public short Zero => 0;

            public string OverflowMessage => SR.Overflow_Int16;

            public short And(short left, short right) => (short)(left & right);

            public int CompareTo(short left, short right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => value <= (ulong)short.MaxValue || value >= unchecked((ulong)short.MinValue);

            public short LeftShift(short value, int amount) => (short)(value << amount);

            public bool LessThan(short left, short right) => left < right;

            public short Not(short value) => (short)~value;

            public short Or(short left, short right) => (short)(left | right);

            public short Subtract(short left, short right) => (short)(left - right);

            public bool ToBoolean(short value) => Convert.ToBoolean(value);

            public byte ToByte(short value) => Convert.ToByte(value);

            public char ToChar(short value) => Convert.ToChar(value);

            public decimal ToDecimal(short value) => value;

            public double ToDouble(short value) => value;

            public string ToHexString(short value) => value.ToString("X4");

            public short ToInt16(short value) => value;

            public int ToInt32(short value) => value;

            public long ToInt64(short value) => value;

            public short ToObject(ulong value) => (short)value;

            public sbyte ToSByte(short value) => Convert.ToSByte(value);

            public float ToSingle(short value) => value;

            public ushort ToUInt16(short value) => Convert.ToUInt16(value);

            public uint ToUInt32(short value) => Convert.ToUInt32(value);

            public ulong ToUInt64(short value) => Convert.ToUInt64(value);

            public ulong ToUInt64Unchecked(short value) => (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out short result) => short.TryParse(span, NumberStyles.Integer, null, out result);
        }

        private struct UInt16Operators : IUnderlyingOperators<ushort>
        {
            public ushort One => 1;

            public ushort Zero => 0;

            public string OverflowMessage => SR.Overflow_UInt16;

            public ushort And(ushort left, ushort right) => (ushort)(left & right);

            public int CompareTo(ushort left, ushort right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => value <= ushort.MaxValue;

            public ushort LeftShift(ushort value, int amount) => (ushort)(value << amount);

            public bool LessThan(ushort left, ushort right) => left < right;

            public ushort Not(ushort value) => (ushort)~value;

            public ushort Or(ushort left, ushort right) => (ushort)(left | right);

            public ushort Subtract(ushort left, ushort right) => (ushort)(left - right);

            public bool ToBoolean(ushort value) => Convert.ToBoolean(value);

            public byte ToByte(ushort value) => Convert.ToByte(value);

            public char ToChar(ushort value) => (char)value;

            public decimal ToDecimal(ushort value) => value;

            public double ToDouble(ushort value) => value;

            public string ToHexString(ushort value) => value.ToString("X4");

            public short ToInt16(ushort value) => Convert.ToInt16(value);

            public int ToInt32(ushort value) => value;

            public long ToInt64(ushort value) => value;

            public ushort ToObject(ulong value) => (ushort)value;

            public sbyte ToSByte(ushort value) => Convert.ToSByte(value);

            public float ToSingle(ushort value) => value;

            public ushort ToUInt16(ushort value) => value;

            public uint ToUInt32(ushort value) => value;

            public ulong ToUInt64(ushort value) => value;

            public ulong ToUInt64Unchecked(ushort value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out ushort result) => ushort.TryParse(span, NumberStyles.Integer, null, out result);
        }

        private struct Int32Operators : IUnderlyingOperators<int>
        {
            public int One => 1;

            public int Zero => 0;

            public string OverflowMessage => SR.Overflow_Int32;

            public int And(int left, int right) => left & right;

            public int CompareTo(int left, int right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => value <= int.MaxValue || value >= unchecked((ulong)int.MinValue);

            public int LeftShift(int value, int amount) => value << amount;

            public bool LessThan(int left, int right) => left < right;

            public int Not(int value) => ~value;

            public int Or(int left, int right) => left | right;

            public int Subtract(int left, int right) => left - right;

            public bool ToBoolean(int value) => Convert.ToBoolean(value);

            public byte ToByte(int value) => Convert.ToByte(value);

            public char ToChar(int value) => Convert.ToChar(value);

            public decimal ToDecimal(int value) => value;

            public double ToDouble(int value) => value;

            public string ToHexString(int value) => value.ToString("X8");

            public short ToInt16(int value) => Convert.ToInt16(value);

            public int ToInt32(int value) => value;

            public long ToInt64(int value) => value;

            public int ToObject(ulong value) => (int)value;

            public sbyte ToSByte(int value) => Convert.ToSByte(value);

            public float ToSingle(int value) => value;

            public ushort ToUInt16(int value) => Convert.ToUInt16(value);

            public uint ToUInt32(int value) => Convert.ToUInt32(value);

            public ulong ToUInt64(int value) => Convert.ToUInt64(value);

            public ulong ToUInt64Unchecked(int value) => (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out int result) => int.TryParse(span, NumberStyles.Integer, null, out result);
        }

        private struct UInt32Operators : IUnderlyingOperators<uint>
        {
            public uint One => 1;

            public uint Zero => 0;

            public string OverflowMessage => SR.Overflow_UInt32;

            public uint And(uint left, uint right) => left & right;

            public int CompareTo(uint left, uint right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => value <= uint.MaxValue;

            public uint LeftShift(uint value, int amount) => value << amount;

            public bool LessThan(uint left, uint right) => left < right;

            public uint Not(uint value) => ~value;

            public uint Or(uint left, uint right) => left | right;

            public uint Subtract(uint left, uint right) => left - right;

            public bool ToBoolean(uint value) => Convert.ToBoolean(value);

            public byte ToByte(uint value) => Convert.ToByte(value);

            public char ToChar(uint value) => Convert.ToChar(value);

            public decimal ToDecimal(uint value) => value;

            public double ToDouble(uint value) => value;

            public string ToHexString(uint value) => value.ToString("X8");

            public short ToInt16(uint value) => Convert.ToInt16(value);

            public int ToInt32(uint value) => Convert.ToInt32(value);

            public long ToInt64(uint value) => value;

            public uint ToObject(ulong value) => (uint)value;

            public sbyte ToSByte(uint value) => Convert.ToSByte(value);

            public float ToSingle(uint value) => value;

            public ushort ToUInt16(uint value) => Convert.ToUInt16(value);

            public uint ToUInt32(uint value) => value;

            public ulong ToUInt64(uint value) => value;

            public ulong ToUInt64Unchecked(uint value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out uint result) => uint.TryParse(span, NumberStyles.Integer, null, out result);
        }

        private struct Int64Operators : IUnderlyingOperators<long>
        {
            public long One => 1;

            public long Zero => 0;

            public string OverflowMessage => SR.Overflow_Int64;

            public long And(long left, long right) => left & right;

            public int CompareTo(long left, long right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => true;

            public long LeftShift(long value, int amount) => value << amount;

            public bool LessThan(long left, long right) => left < right;

            public long Not(long value) => ~value;

            public long Or(long left, long right) => left | right;

            public long Subtract(long left, long right) => left - right;

            public bool ToBoolean(long value) => Convert.ToBoolean(value);

            public byte ToByte(long value) => Convert.ToByte(value);

            public char ToChar(long value) => Convert.ToChar(value);

            public decimal ToDecimal(long value) => value;

            public double ToDouble(long value) => value;

            public string ToHexString(long value) => value.ToString("X16");

            public short ToInt16(long value) => Convert.ToInt16(value);

            public int ToInt32(long value) => Convert.ToInt32(value);

            public long ToInt64(long value) => value;

            public long ToObject(ulong value) => (long)value;

            public sbyte ToSByte(long value) => Convert.ToSByte(value);

            public float ToSingle(long value) => value;

            public ushort ToUInt16(long value) => Convert.ToUInt16(value);

            public uint ToUInt32(long value) => Convert.ToUInt32(value);

            public ulong ToUInt64(long value) => Convert.ToUInt64(value);

            public ulong ToUInt64Unchecked(long value) => (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out long result) => long.TryParse(span, NumberStyles.Integer, null, out result);
        }

        private struct UInt64Operators : IUnderlyingOperators<ulong>
        {
            public ulong One => 1;

            public ulong Zero => 0;

            public string OverflowMessage => SR.Overflow_UInt64;

            public ulong And(ulong left, ulong right) => left & right;

            public int CompareTo(ulong left, ulong right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => true;

            public ulong LeftShift(ulong value, int amount) => value << amount;

            public bool LessThan(ulong left, ulong right) => left < right;

            public ulong Not(ulong value) => ~value;

            public ulong Or(ulong left, ulong right) => left | right;

            public ulong Subtract(ulong left, ulong right) => left - right;

            public bool ToBoolean(ulong value) => Convert.ToBoolean(value);

            public byte ToByte(ulong value) => Convert.ToByte(value);

            public char ToChar(ulong value) => Convert.ToChar(value);

            public decimal ToDecimal(ulong value) => value;

            public double ToDouble(ulong value) => value;

            public string ToHexString(ulong value) => value.ToString("X16");

            public short ToInt16(ulong value) => Convert.ToInt16(value);

            public int ToInt32(ulong value) => Convert.ToInt32(value);

            public long ToInt64(ulong value) => Convert.ToInt64(value);

            public ulong ToObject(ulong value) => value;

            public sbyte ToSByte(ulong value) => Convert.ToSByte(value);

            public float ToSingle(ulong value) => value;

            public ushort ToUInt16(ulong value) => Convert.ToUInt16(value);

            public uint ToUInt32(ulong value) => Convert.ToUInt32(value);

            public ulong ToUInt64(ulong value) => value;

            public ulong ToUInt64Unchecked(ulong value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out ulong result) => ulong.TryParse(span, NumberStyles.Integer, null, out result);
        }

        private struct BooleanOperators : IUnderlyingOperators<bool>
        {
            public bool One => true;

            public bool Zero => false;

            public string OverflowMessage => null;

            public bool And(bool left, bool right) => left & right;

            public int CompareTo(bool left, bool right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => value <= bool.True;

            public bool LeftShift(bool value, int amount) => !value;

            public bool LessThan(bool left, bool right) => !left & right;

            public bool Not(bool value) => !value;

            public bool Or(bool left, bool right) => left | right;

            public bool Subtract(bool left, bool right) => left ^ right;

            public bool ToBoolean(bool value) => value;

            public byte ToByte(bool value) => Convert.ToByte(value);

            public char ToChar(bool value) => Convert.ToChar(value);

            public decimal ToDecimal(bool value) => Convert.ToDecimal(value);

            public double ToDouble(bool value) => Convert.ToDouble(value);

            public string ToHexString(bool value) => Convert.ToByte(value).ToString("X2");

            public short ToInt16(bool value) => Convert.ToInt16(value);

            public int ToInt32(bool value) => Convert.ToInt32(value);

            public long ToInt64(bool value) => Convert.ToInt64(value);

            public bool ToObject(ulong value) => value != 0UL;

            public sbyte ToSByte(bool value) => Convert.ToSByte(value);

            public float ToSingle(bool value) => Convert.ToSingle(value);

            public ushort ToUInt16(bool value) => Convert.ToUInt16(value);

            public uint ToUInt32(bool value) => Convert.ToUInt32(value);

            public ulong ToUInt64(bool value) => Convert.ToUInt64(value);

            public ulong ToUInt64Unchecked(bool value) => Convert.ToUInt64(value);

            public bool TryParse(ReadOnlySpan<char> span, out bool result) => bool.TryParse(span, out result);
        }

        private struct CharOperators : IUnderlyingOperators<char>
        {
            public char One => (char)1;

            public char Zero => (char)0;

            public string OverflowMessage => SR.Overflow_Char;

            public char And(char left, char right) => (char)(left & right);

            public int CompareTo(char left, char right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => value <= char.MaxValue;

            public char LeftShift(char value, int amount) => (char)(value << amount);

            public bool LessThan(char left, char right) => left < right;

            public char Not(char value) => (char)~value;

            public char Or(char left, char right) => (char)(left | right);

            public char Subtract(char left, char right) => (char)(left - right);

            public bool ToBoolean(char value) => Convert.ToBoolean(value);

            public byte ToByte(char value) => Convert.ToByte(value);

            public char ToChar(char value) => value;

            public decimal ToDecimal(char value) => value;

            public double ToDouble(char value) => value;

            public string ToHexString(char value) => ((ushort)value).ToString("X4");

            public short ToInt16(char value) => Convert.ToInt16(value);

            public int ToInt32(char value) => value;

            public long ToInt64(char value) => value;

            public char ToObject(ulong value) => (char)value;

            public sbyte ToSByte(char value) => Convert.ToSByte(value);

            public float ToSingle(char value) => value;

            public ushort ToUInt16(char value) => value;

            public uint ToUInt32(char value) => value;

            public ulong ToUInt64(char value) => value;

            public ulong ToUInt64Unchecked(char value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out char result)
            {
                bool success = span.Length == 1;
                result = success ? span[0] : default;
                return success;
            }
        }

        private struct SingleOperators : IUnderlyingOperators<float>
        {
            public float One => BitConverter.Int32BitsToSingle(1);

            public float Zero => default;

            public string OverflowMessage => SR.Overflow_Single;

            public float And(float left, float right) => BitConverter.Int32BitsToSingle(BitConverter.SingleToInt32Bits(left) & BitConverter.SingleToInt32Bits(right));

            public int CompareTo(float left, float right) => BitConverter.SingleToInt32Bits(left).CompareTo(BitConverter.SingleToInt32Bits(right));

            public bool IsInValueRange(ulong value) => value <= int.MaxValue || value >= unchecked((ulong)int.MinValue);

            public float LeftShift(float value, int amount) => BitConverter.Int32BitsToSingle(BitConverter.SingleToInt32Bits(value) << amount);

            public bool LessThan(float left, float right) => BitConverter.SingleToInt32Bits(left) < BitConverter.SingleToInt32Bits(right);

            public float Not(float value) => BitConverter.Int32BitsToSingle(~BitConverter.SingleToInt32Bits(value));

            public float Or(float left, float right) => BitConverter.Int32BitsToSingle(BitConverter.SingleToInt32Bits(left) | BitConverter.SingleToInt32Bits(right));

            public float Subtract(float left, float right) => BitConverter.Int32BitsToSingle(BitConverter.SingleToInt32Bits(left) - BitConverter.SingleToInt32Bits(right));

            public bool ToBoolean(float value) => Convert.ToBoolean(value);

            public byte ToByte(float value) => Convert.ToByte(value);

            public char ToChar(float value) => Convert.ToChar(value);

            public decimal ToDecimal(float value) => (decimal)value;

            public double ToDouble(float value) => value;

            public string ToHexString(float value) => BitConverter.SingleToInt32Bits(value).ToString("X8");

            public short ToInt16(float value) => Convert.ToInt16(value);

            public int ToInt32(float value) => Convert.ToInt32(value);

            public long ToInt64(float value) => Convert.ToInt64(value);

            public float ToObject(ulong value) => BitConverter.Int32BitsToSingle((int)value);

            public sbyte ToSByte(float value) => Convert.ToSByte(value);

            public float ToSingle(float value) => value;

            public ushort ToUInt16(float value) => Convert.ToUInt16(value);

            public uint ToUInt32(float value) => Convert.ToUInt32(value);

            public ulong ToUInt64(float value) => Convert.ToUInt64(value);

            public ulong ToUInt64Unchecked(float value) => (ulong)BitConverter.SingleToInt32Bits(value);

            public bool TryParse(ReadOnlySpan<char> span, out float result) => float.TryParse(span, NumberStyles.Float, null, out result);
        }

        private struct DoubleOperators : IUnderlyingOperators<double>
        {
            public double One => BitConverter.Int64BitsToDouble(1);

            public double Zero => default;

            public string OverflowMessage => SR.Overflow_Double;

            public double And(double left, double right) => BitConverter.Int64BitsToDouble(BitConverter.DoubleToInt64Bits(left) & BitConverter.DoubleToInt64Bits(right));

            public int CompareTo(double left, double right) => left.CompareTo(right);

            public bool IsInValueRange(ulong value) => true;

            public double LeftShift(double value, int amount) => BitConverter.Int64BitsToDouble(BitConverter.DoubleToInt64Bits(value) << amount);

            public bool LessThan(double left, double right) => BitConverter.DoubleToInt64Bits(left) < BitConverter.DoubleToInt64Bits(right);

            public double Not(double value) => BitConverter.Int64BitsToDouble(~BitConverter.DoubleToInt64Bits(value));

            public double Or(double left, double right) => BitConverter.Int64BitsToDouble(BitConverter.DoubleToInt64Bits(left) | BitConverter.DoubleToInt64Bits(right));

            public double Subtract(double left, double right) => BitConverter.Int64BitsToDouble(BitConverter.DoubleToInt64Bits(left) - BitConverter.DoubleToInt64Bits(right));

            public bool ToBoolean(double value) => Convert.ToBoolean(value);

            public byte ToByte(double value) => Convert.ToByte(value);

            public char ToChar(double value) => Convert.ToChar(value);

            public decimal ToDecimal(double value) => (decimal)value;

            public double ToDouble(double value) => value;

            public string ToHexString(double value) => BitConverter.DoubleToInt64Bits(value).ToString("X16");

            public short ToInt16(double value) => Convert.ToInt16(value);

            public int ToInt32(double value) => Convert.ToInt32(value);

            public long ToInt64(double value) => Convert.ToInt64(value);

            public double ToObject(ulong value) => BitConverter.Int64BitsToDouble((long)value);

            public sbyte ToSByte(double value) => Convert.ToSByte(value);

            public float ToSingle(double value) => (float)value;

            public ushort ToUInt16(double value) => Convert.ToUInt16(value);

            public uint ToUInt32(double value) => Convert.ToUInt32(value);

            public ulong ToUInt64(double value) => Convert.ToUInt64(value);

            public ulong ToUInt64Unchecked(double value) => (ulong)BitConverter.DoubleToInt64Bits(value);

            public bool TryParse(ReadOnlySpan<char> span, out double result) => double.TryParse(span, NumberStyles.Float, null, out result);
        }

        private struct IntPtrOperators : IUnderlyingOperators<IntPtr>
        {
            public IntPtr One => (IntPtr)1;

            public IntPtr Zero => IntPtr.Zero;

            public string OverflowMessage => IntPtr.Size == 4 ? SR.Overflow_Int32 : SR.Overflow_Int64;

            public IntPtr And(IntPtr left, IntPtr right) => IntPtr.Size == 4 ? (IntPtr)((int)left & (int)right) : (IntPtr)((long)left & (long)right);

            public int CompareTo(IntPtr left, IntPtr right) => IntPtr.Size == 4 ? ((int)left).CompareTo((int)right) : ((long)left).CompareTo((long)right);

            public bool IsInValueRange(ulong value) => IntPtr.Size == 8 || (value <= int.MaxValue || value >= unchecked((ulong)int.MinValue));

            public IntPtr LeftShift(IntPtr value, int amount) => IntPtr.Size == 4 ? (IntPtr)((int)value << amount) : (IntPtr)((long)value << amount);

            public bool LessThan(IntPtr left, IntPtr right) => IntPtr.Size == 4 ? (int)left < (int)right : (long)left < (long)right;

            public IntPtr Not(IntPtr value) => IntPtr.Size == 4 ? (IntPtr)~(int)value : (IntPtr)~(long)value;

            public IntPtr Or(IntPtr left, IntPtr right) => IntPtr.Size == 4 ? (IntPtr)((int)left | (int)right) : (IntPtr)((long)left | (long)right);

            public IntPtr Subtract(IntPtr left, IntPtr right) => IntPtr.Size == 4 ? (IntPtr)((int)left - (int)right) : (IntPtr)((long)left - (long)right);

            public bool ToBoolean(IntPtr value) => IntPtr.Size == 4 ? Convert.ToBoolean((int)value) : Convert.ToBoolean((long)value);

            public byte ToByte(IntPtr value) => IntPtr.Size == 4 ? Convert.ToByte((int)value) : Convert.ToByte((long)value);

            public char ToChar(IntPtr value) => IntPtr.Size == 4 ? Convert.ToChar((int)value) : Convert.ToChar((long)value);

            public decimal ToDecimal(IntPtr value) => IntPtr.Size == 4 ? (int)value : (long)value;

            public double ToDouble(IntPtr value) => IntPtr.Size == 4 ? (int)value : (long)value;

            public string ToHexString(IntPtr value) => IntPtr.Size == 4 ? ((int)value).ToString("X8") : ((long)value).ToString("X16");

            public short ToInt16(IntPtr value) => IntPtr.Size == 4 ? Convert.ToInt16((int)value) : Convert.ToInt16((long)value);

            public int ToInt32(IntPtr value) => IntPtr.Size == 4 ? (int)value : Convert.ToInt32((long)value);

            public long ToInt64(IntPtr value) => IntPtr.Size == 4 ? (int)value : (long)value;

            public IntPtr ToObject(ulong value) => IntPtr.Size == 4 ? (IntPtr)(int)value : (IntPtr)(long)value;

            public sbyte ToSByte(IntPtr value) => IntPtr.Size == 4 ? Convert.ToSByte((int)value) : Convert.ToSByte((long)value);

            public float ToSingle(IntPtr value) => IntPtr.Size == 4 ? (int)value : (long)value;

            public ushort ToUInt16(IntPtr value) => IntPtr.Size == 4 ? Convert.ToUInt16((int)value) : Convert.ToUInt16((long)value);

            public uint ToUInt32(IntPtr value) => IntPtr.Size == 4 ? Convert.ToUInt32((int)value) : Convert.ToUInt32((long)value);

            public ulong ToUInt64(IntPtr value) => IntPtr.Size == 4 ? Convert.ToUInt64((int)value) : Convert.ToUInt64((long)value);

            public ulong ToUInt64Unchecked(IntPtr value) => IntPtr.Size == 4 ? (ulong)(int)value : (ulong)(long)value;

            public bool TryParse(ReadOnlySpan<char> span, out IntPtr result)
            {
                bool success;
                if (IntPtr.Size == 4)
                {
                    success = int.TryParse(span, NumberStyles.Integer, null, out int int32Result);
                    result = (IntPtr)int32Result;
                }
                else
                {
                    success = long.TryParse(span, NumberStyles.Integer, null, out long int64Result);
                    result = (IntPtr)int64Result;
                }
                return success;
            }
        }

        private struct UIntPtrOperators : IUnderlyingOperators<UIntPtr>
        {
            public UIntPtr One => (UIntPtr)1;

            public UIntPtr Zero => UIntPtr.Zero;

            public string OverflowMessage => UIntPtr.Size == 4 ? SR.Overflow_UInt32 : SR.Overflow_UInt64;

            public UIntPtr And(UIntPtr left, UIntPtr right) => UIntPtr.Size == 4 ? (UIntPtr)((uint)left & (uint)right) : (UIntPtr)((ulong)left & (ulong)right);

            public int CompareTo(UIntPtr left, UIntPtr right) => UIntPtr.Size == 4 ? ((uint)left).CompareTo((uint)right) : ((ulong)left).CompareTo((ulong)right);

            public bool IsInValueRange(ulong value) => UIntPtr.Size == 8 || value <= uint.MaxValue;

            public UIntPtr LeftShift(UIntPtr value, int amount) => UIntPtr.Size == 4 ? (UIntPtr)((uint)value << amount) : (UIntPtr)((ulong)value << amount);

            public bool LessThan(UIntPtr left, UIntPtr right) => UIntPtr.Size == 4 ? (uint)left < (uint)right : (ulong)left < (ulong)right;

            public UIntPtr Not(UIntPtr value) => UIntPtr.Size == 4 ? (UIntPtr)~(uint)value : (UIntPtr)~(ulong)value;

            public UIntPtr Or(UIntPtr left, UIntPtr right) => UIntPtr.Size == 4 ? (UIntPtr)((uint)left | (uint)right) : (UIntPtr)((ulong)left | (ulong)right);

            public UIntPtr Subtract(UIntPtr left, UIntPtr right) => UIntPtr.Size == 4 ? (UIntPtr)((uint)left - (uint)right) : (UIntPtr)((ulong)left - (ulong)right);

            public bool ToBoolean(UIntPtr value) => UIntPtr.Size == 4 ? Convert.ToBoolean((uint)value) : Convert.ToBoolean((ulong)value);

            public byte ToByte(UIntPtr value) => UIntPtr.Size == 4 ? Convert.ToByte((uint)value) : Convert.ToByte((ulong)value);

            public char ToChar(UIntPtr value) => UIntPtr.Size == 4 ? Convert.ToChar((uint)value) : Convert.ToChar((ulong)value);

            public decimal ToDecimal(UIntPtr value) => UIntPtr.Size == 4 ? (uint)value : (ulong)value;

            public double ToDouble(UIntPtr value) => UIntPtr.Size == 4 ? (uint)value : (ulong)value;

            public string ToHexString(UIntPtr value) => UIntPtr.Size == 4 ? ((uint)value).ToString("X8") : ((ulong)value).ToString("X16");

            public short ToInt16(UIntPtr value) => UIntPtr.Size == 4 ? Convert.ToInt16((uint)value) : Convert.ToInt16((ulong)value);

            public int ToInt32(UIntPtr value) => UIntPtr.Size == 4 ? Convert.ToInt32((uint)value) : Convert.ToInt32((ulong)value);

            public long ToInt64(UIntPtr value) => UIntPtr.Size == 4 ? Convert.ToInt64((uint)value) : Convert.ToInt64((ulong)value);

            public UIntPtr ToObject(ulong value) => UIntPtr.Size == 4 ? (UIntPtr)(uint)value : (UIntPtr)value;

            public sbyte ToSByte(UIntPtr value) => UIntPtr.Size == 4 ? Convert.ToSByte((uint)value) : Convert.ToSByte((ulong)value);

            public float ToSingle(UIntPtr value) => UIntPtr.Size == 4 ? (uint)value : (ulong)value;

            public ushort ToUInt16(UIntPtr value) => UIntPtr.Size == 4 ? Convert.ToUInt16((uint)value) : Convert.ToUInt16((ulong)value);

            public uint ToUInt32(UIntPtr value) => UIntPtr.Size == 4 ? (uint)value : Convert.ToUInt32((ulong)value);

            public ulong ToUInt64(UIntPtr value) => UIntPtr.Size == 4 ? (uint)value : (ulong)value;

            public ulong ToUInt64Unchecked(UIntPtr value) => UIntPtr.Size == 4 ? (uint)value : (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out UIntPtr result)
            {
                bool success;
                if (UIntPtr.Size == 4)
                {
                    success = uint.TryParse(span, NumberStyles.Integer, null, out uint uint32Result);
                    result = (UIntPtr)uint32Result;
                }
                else
                {
                    success = ulong.TryParse(span, NumberStyles.Integer, null, out ulong uint64Result);
                    result = (UIntPtr)uint64Result;
                }
                return success;
            }
        }
        #endregion
        #endregion

        #region Private Methods
        internal object GetUnderlyingValue() => Bridge.GetUnderlyingValue(this);

        private IEnumBridge Bridge => GetBridge((RuntimeType)GetType());
        #endregion

        #region Object Overrides
        public override bool Equals(object obj) => Bridge.Equals(this, obj);

        public static bool Equals<TEnum>(TEnum value, TEnum other) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.Equals(value, other);

        public override int GetHashCode() => Bridge.GetHashCode(this);

        internal static int GetHashCode<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.GetHashCode(value);

        public override string ToString() => Bridge.ToString(this);

        public static string ToString<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToString(value);
        #endregion

        #region IFormattable
        [Obsolete("The provider argument is not used. Please use ToString(String).")]
        public string ToString(string format, IFormatProvider provider) => ToString(format);
        #endregion

        #region IComparable
        public int CompareTo(object target) => target != null ? Bridge.CompareTo(this, target) : 1;

        public static int CompareTo<TEnum>(TEnum value, TEnum other) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.CompareTo(value, other);
        #endregion

        #region Public Methods
        public string ToString(string format) => Bridge.ToString(this, format);

        public static string ToString<TEnum>(TEnum value, string format) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToString(value, format);

        [Obsolete("The provider argument is not used. Please use ToString().")]
        public string ToString(IFormatProvider provider) => ToString();

        [Intrinsic]
        [Obsolete("Please use System.Flags.FlagEnum's HasAllFlags or HasAnyFlags method instead.")]
        [EditorBrowsable(EditorBrowsableState.Never)]
        public bool HasFlag(Enum flag)
        {
            if (flag == null)
            {
                throw new ArgumentNullException(nameof(flag));
            }

            return Bridge.HasAllFlags(this, flag);
        }
        #endregion

        #region IConvertable
        public TypeCode GetTypeCode() => Bridge.TypeCode;

        bool IConvertible.ToBoolean(IFormatProvider provider) => Bridge.ToBoolean(this);

        char IConvertible.ToChar(IFormatProvider provider) => Bridge.ToChar(this);

        sbyte IConvertible.ToSByte(IFormatProvider provider) => Bridge.ToSByte(this);

        [CLSCompliant(false)]
        public static sbyte ToSByte<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToSByte(value);

        byte IConvertible.ToByte(IFormatProvider provider) => Bridge.ToByte(this);
        
        public static byte ToByte<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToByte(value);

        short IConvertible.ToInt16(IFormatProvider provider) => Bridge.ToInt16(this);
        
        public static short ToInt16<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToInt16(value);

        ushort IConvertible.ToUInt16(IFormatProvider provider) => Bridge.ToUInt16(this);

        [CLSCompliant(false)]
        public static ushort ToUInt16<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToUInt16(value);

        int IConvertible.ToInt32(IFormatProvider provider) => Bridge.ToInt32(this);

        public static int ToInt32<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToInt32(value);

        uint IConvertible.ToUInt32(IFormatProvider provider) => Bridge.ToUInt32(this);

        [CLSCompliant(false)]
        public static uint ToUInt32<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToUInt32(value);

        long IConvertible.ToInt64(IFormatProvider provider) => Bridge.ToInt64(this);

        public static long ToInt64<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToInt64(value);

        ulong IConvertible.ToUInt64(IFormatProvider provider) => Bridge.ToUInt64(this);

        [CLSCompliant(false)]
        public static ulong ToUInt64<TEnum>(TEnum value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToUInt64(value);

        float IConvertible.ToSingle(IFormatProvider provider) => Bridge.ToSingle(this);

        double IConvertible.ToDouble(IFormatProvider provider) => Bridge.ToDouble(this);

        decimal IConvertible.ToDecimal(IFormatProvider provider) => Bridge.ToDecimal(this);

        DateTime IConvertible.ToDateTime(IFormatProvider provider) => throw new InvalidCastException(SR.Format(SR.InvalidCast_FromTo, "Enum", "DateTime"));

        object IConvertible.ToType(Type type, IFormatProvider provider) => Convert.DefaultToType(this, type, provider);
        #endregion

        #region ToObject
        [CLSCompliant(false)]
        public static object ToObject(Type enumType, sbyte value) => GetBridge(enumType).ToObject((ulong)value);

        [CLSCompliant(false)]
        public static TEnum ToObject<TEnum>(sbyte value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject((ulong)value);

        public static object ToObject(Type enumType, short value) => GetBridge(enumType).ToObject((ulong)value);
        
        public static TEnum ToObject<TEnum>(short value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject((ulong)value);

        public static object ToObject(Type enumType, int value) => GetBridge(enumType).ToObject((ulong)value);
        
        public static TEnum ToObject<TEnum>(int value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject((ulong)value);

        public static object ToObject(Type enumType, byte value) => GetBridge(enumType).ToObject(value);
        
        public static TEnum ToObject<TEnum>(byte value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);

        [CLSCompliant(false)]
        public static object ToObject(Type enumType, ushort value) => GetBridge(enumType).ToObject(value);

        [CLSCompliant(false)]
        public static TEnum ToObject<TEnum>(ushort value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);

        [CLSCompliant(false)]
        public static object ToObject(Type enumType, uint value) => GetBridge(enumType).ToObject(value);

        [CLSCompliant(false)]
        public static TEnum ToObject<TEnum>(uint value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);

        public static object ToObject(Type enumType, long value) => GetBridge(enumType).ToObject((ulong)value);
        
        public static TEnum ToObject<TEnum>(long value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject((ulong)value);

        [CLSCompliant(false)]
        public static object ToObject(Type enumType, ulong value) => GetBridge(enumType).ToObject(value);

        [CLSCompliant(false)]
        public static TEnum ToObject<TEnum>(ulong value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);
        #endregion
    }
}
