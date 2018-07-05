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

        private static IEnumBridge GetBridge(RuntimeType rtType)
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

        public static bool TryParse<TEnum>(string value, out TEnum result) where TEnum : struct, Enum => TryParse(value, false, out result);

        public static bool TryParse<TEnum>(string value, bool ignoreCase, out TEnum result) where TEnum : struct, Enum => TryParse(value.AsSpan(), ignoreCase, out result);

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

        public static TEnum Parse<TEnum>(string value) where TEnum : struct, Enum => Parse<TEnum>(value, false);

        public static TEnum Parse<TEnum>(string value, bool ignoreCase) where TEnum : struct, Enum
        {
            if (value == null)
            {
                throw new ArgumentNullException(nameof(value));
            }

            return Parse<TEnum>(value.AsSpan(), ignoreCase);
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
        internal static class EnumBridge<TEnum> where TEnum : struct, Enum
        {
            public readonly static IEnumBridge<TEnum> Bridge = CreateEnumBridge();

            private static IEnumBridge<TEnum> CreateEnumBridge()
            {
                Type underlyingType = GetUnderlyingTypeInternal(typeof(TEnum));
                switch (Type.GetTypeCode(underlyingType))
                {
                    case TypeCode.SByte:
                        return new EnumBridge<TEnum, sbyte, SByteOperators>();
                    case TypeCode.Byte:
                        return new EnumBridge<TEnum, byte, ByteOperators>();
                    case TypeCode.Int16:
                        return new EnumBridge<TEnum, short, Int16Operators>();
                    case TypeCode.UInt16:
                        return new EnumBridge<TEnum, ushort, UInt16Operators>();
                    case TypeCode.Int32:
                        return new EnumBridge<TEnum, int, Int32Operators>();
                    case TypeCode.UInt32:
                        return new EnumBridge<TEnum, uint, UInt32Operators>();
                    case TypeCode.Int64:
                        return new EnumBridge<TEnum, long, Int64Operators>();
                    case TypeCode.UInt64:
                        return new EnumBridge<TEnum, ulong, UInt64Operators>();
                    case TypeCode.Boolean:
                        return new EnumBridge<TEnum, bool, BooleanOperators>();
                    case TypeCode.Char:
                        return new EnumBridge<TEnum, char, CharOperators>();
                    default:
                        return null;
                }
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
            object ToObject(long value);
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

        internal interface IEnumBridge<TEnum> : IEnumBridgeCommon where TEnum : struct, Enum
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
            TEnum ToObject(long value);
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
            where TUnderlying : struct, IComparable<TUnderlying>, IEquatable<TUnderlying>, IConvertible
            where TUnderlyingOperators : struct, IUnderlyingOperators<TUnderlying>
        {
            private static readonly TUnderlyingOperators s_operators = new TUnderlyingOperators();

            private static readonly EnumCache<TUnderlying, TUnderlyingOperators> s_cache = new EnumCache<TUnderlying, TUnderlyingOperators>(typeof(TEnum));

            private static TUnderlying ToUnderlying(TEnum value) => Unsafe.As<TEnum, TUnderlying>(ref value);

            private static TEnum ToEnum(TUnderlying value) => Unsafe.As<TUnderlying, TEnum>(ref value);

            public TEnum Parse(ReadOnlySpan<char> value, bool ignoreCase) => ToEnum(s_cache.Parse(value, ignoreCase));

            public bool TryParse(ReadOnlySpan<char> value, bool ignoreCase, out TEnum result)
            {
                bool success = s_cache.TryParse(value, ignoreCase, out TUnderlying underlying);
                result = ToEnum(underlying);
                return success;
            }

            public int CompareTo(TEnum value, TEnum other) => ToUnderlying(value).CompareTo(ToUnderlying(other));

            public bool Equals(TEnum value, TEnum other) => ToUnderlying(value).Equals(ToUnderlying(other));

            public int GetHashCode(TEnum value) => ToUnderlying(value).GetHashCode();

            public string Format(TEnum value, string format) => s_cache.Format(ToUnderlying(value), format);

            public string GetName(TEnum value) => s_cache.GetName(ToUnderlying(value));

            public IEnumerable<string> GetNames() => s_cache.GetNames();

            public Type UnderlyingType { get; } = typeof(TUnderlying);

            public TypeCode TypeCode { get; } = Type.GetTypeCode(typeof(TUnderlying));

            public IEnumerable<TEnum> GetValues()
            {
                int dupeIndex = 0;
                List<EnumCache<TUnderlying, TUnderlyingOperators>.EnumMemberInternal> duplicateValues = s_cache._duplicateValues;
                TUnderlying nextDupeValue = duplicateValues != null ? duplicateValues[dupeIndex].Value : default;
                foreach (KeyValuePair<TUnderlying, EnumCache<TUnderlying, TUnderlyingOperators>.EnumMemberInternal> pair in s_cache._valueMap)
                {
                    yield return ToEnum(pair.Key);
                    while (dupeIndex < duplicateValues?.Count && nextDupeValue.Equals(pair.Key))
                    {
                        yield return ToEnum(nextDupeValue);
                        ++dupeIndex;
                        if (dupeIndex < duplicateValues.Count)
                        {
                            nextDupeValue = duplicateValues[dupeIndex].Value;
                        }
                    }
                }
            }

            public bool IsDefined(TEnum value) => s_cache.IsDefined(ToUnderlying(value));

            public byte ToByte(TEnum value) => ToUnderlying(value).ToByte(CultureInfo.CurrentCulture);

            public short ToInt16(TEnum value) => ToUnderlying(value).ToInt16(CultureInfo.CurrentCulture);

            public int ToInt32(TEnum value) => ToUnderlying(value).ToInt32(CultureInfo.CurrentCulture);

            public long ToInt64(TEnum value) => ToUnderlying(value).ToInt64(CultureInfo.CurrentCulture);

            public TEnum ToObject(object value) => ToEnum(s_cache.ToObject(value));

            public TEnum ToObject(long value) => ToEnum(s_cache.ToObject(value));

            public TEnum ToObject(ulong value) => ToEnum(s_cache.ToObject(value));

            public sbyte ToSByte(TEnum value) => ToUnderlying(value).ToSByte(CultureInfo.CurrentCulture);

            public string ToString(TEnum value) => s_cache.ToString(ToUnderlying(value));

            public string ToString(TEnum value, string format) => s_cache.ToString(ToUnderlying(value), format);

            public ushort ToUInt16(TEnum value) => ToUnderlying(value).ToUInt16(CultureInfo.CurrentCulture);

            public uint ToUInt32(TEnum value) => ToUnderlying(value).ToUInt32(CultureInfo.CurrentCulture);

            public ulong ToUInt64(TEnum value) => ToUnderlying(value).ToUInt64(CultureInfo.CurrentCulture);

            public TEnum AllFlags => ToEnum(s_cache._allFlags);

            public TEnum CombineFlags(TEnum value, TEnum flags) => ToEnum(s_operators.Or(ToUnderlying(value), ToUnderlying(flags)));

            public TEnum CombineFlags(IEnumerable<TEnum> flags)
            {
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

            public bool HasAllFlags(TEnum value, TEnum flags) => s_operators.Or(ToUnderlying(value), ToUnderlying(flags)).Equals(ToUnderlying(flags));

            public bool HasAnyFlags(TEnum value) => !ToUnderlying(value).Equals(s_operators.Zero);

            public bool HasAnyFlags(TEnum value, TEnum flags) => !s_operators.And(ToUnderlying(value), ToUnderlying(flags)).Equals(s_operators.Zero);

            public bool IsFlagEnum => s_cache._isFlagEnum;

            public bool IsValidFlagCombination(TEnum value) => s_operators.And(ToUnderlying(value), s_cache._allFlags).Equals(ToUnderlying(value));

            public TEnum RemoveFlags(TEnum value, TEnum flags) => ToEnum(s_operators.And(ToUnderlying(value), s_operators.Not(ToUnderlying(flags))));

            public int Count => s_cache._valueMap.Count + (s_cache._duplicateValues?.Count ?? 0);

            #region IEnumBridge
            private static TEnum ToEnum(object value)
            {
                // Not null validation should already be handled
                if (!(value is TEnum enumValue))
                {
                    throw new ArgumentException(SR.Format(SR.Arg_EnumAndObjectMustBeSameType, value.GetType().ToString(), typeof(TEnum).ToString()));
                }
                return enumValue;
            }

            object IEnumBridge.AllFlags => AllFlags;

            public object CombineFlags(IEnumerable<object> flags)
            {
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

            public int CompareTo(Enum value, object other) => CompareTo((TEnum)value, ToEnum(other));

            public bool Equals(Enum value, object other) => value is TEnum enumValue ? Equals((TEnum)value, enumValue) : false;

            public string Format(object value, string format) => Format(ToEnum(value), format);

            public IEnumerable GetFlags(object value) => GetFlags(ToEnum(value));

            public int GetHashCode(Enum value) => ToUnderlying((TEnum)value).GetHashCode();

            public string GetName(object value) => GetName(value is TEnum enumValue ? enumValue : ToObject(value));

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

            public bool ToBoolean(Enum value) => ToUnderlying((TEnum)value).ToBoolean(CultureInfo.CurrentCulture);

            public byte ToByte(Enum value) => ToByte((TEnum)value);

            public char ToChar(Enum value) => ToUnderlying((TEnum)value).ToChar(CultureInfo.CurrentCulture);

            public decimal ToDecimal(Enum value) => ToUnderlying((TEnum)value).ToDecimal(CultureInfo.CurrentCulture);

            public double ToDouble(Enum value) => ToUnderlying((TEnum)value).ToDouble(CultureInfo.CurrentCulture);

            public short ToInt16(Enum value) => ToInt16((TEnum)value);

            public int ToInt32(Enum value) => ToInt32((TEnum)value);

            public long ToInt64(Enum value) => ToInt64((TEnum)value);

            object IEnumBridge.ToObject(long value) => ToObject(value);

            object IEnumBridge.ToObject(object value) => ToObject(value);

            object IEnumBridge.ToObject(ulong value) => ToObject(value);

            public sbyte ToSByte(Enum value) => ToSByte((TEnum)value);

            public float ToSingle(Enum value) => ToUnderlying((TEnum)value).ToSingle(CultureInfo.CurrentCulture);

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
            where TUnderlying : struct, IComparable<TUnderlying>, IEquatable<TUnderlying>, IConvertible
            where TUnderlyingOperators : struct, IUnderlyingOperators<TUnderlying>
        {
            internal static readonly TUnderlyingOperators s_operators = new TUnderlyingOperators();

            private readonly Type _enumType;

            internal readonly TUnderlying _allFlags;

            internal readonly bool _isFlagEnum;

            private readonly bool _isContiguous;

            private readonly TUnderlying _maxDefined;

            private readonly TUnderlying _minDefined;

            internal readonly Dictionary<TUnderlying, EnumMemberInternal> _valueMap;

            internal readonly List<EnumMemberInternal> _duplicateValues; // is null when there are no duplicate values

            private Dictionary<int, List<EnumMemberInternal>> _nameMap;

            private Dictionary<int, List<EnumMemberInternal>> NameMap
            {
                get
                {
                    Dictionary<int, List<EnumMemberInternal>> nameMap = _nameMap;
                    if (nameMap == null)
                    {
                        nameMap = new Dictionary<int, List<EnumMemberInternal>>(Count);
                        foreach (KeyValuePair<TUnderlying, EnumMemberInternal> pair in _valueMap)
                        {
                            int hash = CompareInfo.GetIgnoreCaseHash(pair.Value.Name);
                            if (!nameMap.TryGetValue(hash, out List<EnumMemberInternal> list))
                            {
                                list = new List<EnumMemberInternal>();
                                nameMap.Add(hash, list);
                            }
                            list.Add(pair.Value);
                        }
                        if (_duplicateValues != null)
                        {
                            foreach (EnumMemberInternal duplicateValue in _duplicateValues)
                            {
                                int hash = CompareInfo.GetIgnoreCaseHash(duplicateValue.Name);
                                if (!nameMap.TryGetValue(hash, out List<EnumMemberInternal> list))
                                {
                                    list = new List<EnumMemberInternal>();
                                    nameMap.Add(hash, list);
                                }
                                list.Add(duplicateValue);
                            }
                        }
                        _nameMap = nameMap;
                    }
                    return nameMap;
                }
            }

            public int Count => _valueMap.Count + (_duplicateValues?.Count ?? 0);

            public EnumCache(Type enumType)
            {
                _enumType = enumType;
                _isFlagEnum = enumType.IsDefined(typeof(FlagsAttribute), false);

                FieldInfo[] fields = enumType.GetFields(BindingFlags.Public | BindingFlags.Static);
                _valueMap = new Dictionary<TUnderlying, EnumMemberInternal>(fields.Length);
                if (fields.Length == 0)
                {
                    return;
                }
                List<EnumMemberInternal> duplicateValues = new List<EnumMemberInternal>();

                foreach (FieldInfo field in fields)
                {
                    string name = field.Name;
                    TUnderlying value = (TUnderlying)field.GetValue(null);
                    EnumMemberInternal member = new EnumMemberInternal(value, name);
                    if (_valueMap.TryGetValue(value, out EnumMemberInternal existing))
                    {
                        if (field.GetCustomAttribute<PrimaryAttribute>(false) != null)
                        {
                            _valueMap[value] = member;
                            member = existing;
                        }
                        duplicateValues.Add(member);
                    }
                    else
                    {
                        _valueMap.Add(value, member);
                        // Is Power of Two
                        if (s_operators.And(value, s_operators.Subtract(value, s_operators.One)).Equals(s_operators.Zero))
                        {
                            _allFlags = s_operators.Or(_allFlags, value);
                        }
                    }
                }

                bool isInOrder = true;
                EnumMemberInternal previous = default;
                bool isFirst = true;
                foreach (KeyValuePair<TUnderlying, EnumMemberInternal> pair in _valueMap)
                {
                    EnumMemberInternal current = pair.Value;
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
                    List<KeyValuePair<TUnderlying, EnumMemberInternal>> values = new List<KeyValuePair<TUnderlying, EnumMemberInternal>>(_valueMap);
                    values.Sort((first, second) => first.Value.CompareTo(second.Value));
                    _valueMap = new Dictionary<TUnderlying, EnumMemberInternal>(_valueMap.Count);

                    foreach (KeyValuePair<TUnderlying, EnumMemberInternal> pair in values)
                    {
                        _valueMap.Add(pair.Key, pair.Value);
                    }

                    _maxDefined = values[values.Count - 1].Key;
                    _minDefined = values[0].Key;
                }

                _isContiguous = s_operators.Subtract(_maxDefined, s_operators.ToObject(_valueMap.Count - 1)).Equals(_minDefined);

                if (duplicateValues.Count > 0)
                {
                    duplicateValues.Capacity = duplicateValues.Count;
                    // Makes sure is in increasing value order
                    duplicateValues.Sort();
                    _duplicateValues = duplicateValues;
                }
            }

            public IEnumerable<string> GetNames()
            {
                int dupeIndex = 0;
                EnumMemberInternal nextDupeMember = _duplicateValues != null ? _duplicateValues[dupeIndex] : default;
                foreach (KeyValuePair<TUnderlying, EnumMemberInternal> pair in _valueMap)
                {
                    yield return pair.Value.Name;
                    while (dupeIndex < _duplicateValues?.Count && nextDupeMember.CompareTo(pair.Value) == 0)
                    {
                        yield return nextDupeMember.Name;
                        ++dupeIndex;
                        if (dupeIndex < _duplicateValues.Count)
                        {
                            nextDupeMember = _duplicateValues[dupeIndex];
                        }
                    }
                }
            }

            public TUnderlying ToObject(object value)
            {
                if (value == null)
                {
                    throw new ArgumentNullException(nameof(value));
                }

                TypeCode typeCode = Convert.GetTypeCode(value);

                switch (typeCode)
                {
                    case TypeCode.SByte:
                        return ToObject((sbyte)value);
                    case TypeCode.Byte:
                        return ToObject((byte)value);
                    case TypeCode.Int16:
                        return ToObject((short)value);
                    case TypeCode.UInt16:
                        return ToObject((ushort)value);
                    case TypeCode.Int32:
                        return ToObject((int)value);
                    case TypeCode.UInt32:
                        return ToObject((uint)value);
                    case TypeCode.Int64:
                        return ToObject((long)value);
                    case TypeCode.UInt64:
                        return ToObject((ulong)value);
                    case TypeCode.Boolean:
                        return ToObject(Convert.ToByte((bool)value));
                    case TypeCode.Char:
                        return ToObject((char)value);
                    default:
                        throw new ArgumentException(SR.Arg_MustBeEnumBaseTypeOrEnum, nameof(value));
                }
            }

            public TUnderlying ToObject(long value) => s_operators.ToObject(value);

            public TUnderlying ToObject(ulong value) => s_operators.ToObject(value);

            public string GetName(TUnderlying value) => _valueMap.TryGetValue(value, out EnumMemberInternal member) ? member.Name : null;

            public bool IsDefined(TUnderlying value) => _isContiguous ? !(s_operators.LessThan(value, _minDefined) || s_operators.LessThan(_maxDefined, value)) : _valueMap.ContainsKey(value);

            public bool IsDefined(object value)
            {
                switch (value)
                {
                    case TUnderlying underlyingValue:
                        return IsDefined(underlyingValue);
                    case string str:
                        return TryParseInternal(str.AsSpan(), false, out _, out bool isNumeric) && !isNumeric;
                    case null:
                        throw new ArgumentNullException(nameof(value));
                }
                
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

            public string ToString(TUnderlying value)
            {
                if (_isFlagEnum)
                {
                    return ToStringFlags(value);
                }

                if (_valueMap.TryGetValue(value, out EnumMemberInternal member))
                {
                    return member.Name;
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
                StringBuilder sb = StringBuilderCache.Acquire();

                List<EnumMemberInternal> members = new List<EnumMemberInternal>(_valueMap.Values); // Could use an OrderedDictionary to remove allocation
                int index = members.Count - 1;
                bool firstTime = true;
                TUnderlying result = value;

                // We will not optimize this code further to keep it maintainable. There are some boundary checks that can be applied
                // to minimize the comparsions required. This code works the same for the best/worst case. In general the number of
                // items in an enum are sufficiently small and not worth the optimization.
                while (index >= 0)
                {
                    value = members[index].Value;
                    if ((index == 0) && (value.Equals(s_operators.Zero)))
                    {
                        break;
                    }

                    if (s_operators.And(result, value).Equals(value))
                    {
                        result = s_operators.Subtract(result, value);
                        if (!firstTime)
                        {
                            sb.Insert(0, enumSeparatorString);
                        }

                        sb.Insert(0, members[index].Name);
                        firstTime = false;
                    }

                    index--;
                }

                string returnString;
                if (!result.Equals(s_operators.Zero))
                {
                    // We were unable to represent this number as a bitwise or of valid flags
                    returnString = value.ToString();
                }
                else if (value.Equals(s_operators.Zero))
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
                    returnString = sb.ToString(); // Return the string representation
                }

                StringBuilderCache.Release(sb);
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
                    }

                    ReadOnlySpan<char> span = value;
                    
                    while (span.Length > 0)
                    {
                        // Find the next separator, if there is one, otherwise the end of the string.
                        int endIndex = span.IndexOf(enumSeparatorChar);
                        if (endIndex == -1)
                        {
                            endIndex = span.Length;
                        }

                        ReadOnlySpan<char> slice = span.Slice(0, endIndex).Trim();

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
                            return false;
                        }
                        else
                        {
                            result = s_operators.Or(result, foundMember.Value);
                            span = span.Slice(endIndex == span.Length ? endIndex + 1: span.Length);
                        }
                    }

                    isNumeric = false;
                    return true;
                }

                return false;
            }

            public struct EnumMemberInternal : IComparable<EnumMemberInternal>
            {
                public TUnderlying Value;
                public string Name;

                public EnumMemberInternal(TUnderlying value, string name)
                {
                    Value = value;
                    Name = name;
                }

                public int CompareTo(EnumMemberInternal other) => s_operators.ToUInt64(Value).CompareTo(s_operators.ToUInt64(other.Value));
            }
        }
        #endregion

        #region UnderlyingOperators
        private interface IUnderlyingOperators<TUnderlying>
            where TUnderlying : struct, IComparable<TUnderlying>, IEquatable<TUnderlying>, IConvertible
        {
            TUnderlying One { get; }
            TUnderlying Zero { get; }
            string OverflowMessage { get; }
            TUnderlying And(TUnderlying left, TUnderlying right);
            TUnderlying LeftShift(TUnderlying value, int amount);
            bool LessThan(TUnderlying left, TUnderlying right);
            TUnderlying Not(TUnderlying value);
            TUnderlying Or(TUnderlying left, TUnderlying right);
            TUnderlying Subtract(TUnderlying left, TUnderlying right);
            string ToHexString(TUnderlying value);
            TUnderlying ToObject(long value);
            TUnderlying ToObject(ulong value);
            ulong ToUInt64(TUnderlying value);
            bool TryParse(ReadOnlySpan<char> span, out TUnderlying result);
        }

        private struct ByteOperators : IUnderlyingOperators<byte>
        {
            public byte One => 1;

            public byte Zero => 0;

            public string OverflowMessage => SR.Overflow_Byte;

            public byte And(byte left, byte right) => (byte)(left & right);

            public byte LeftShift(byte value, int amount) => (byte)(value << amount);

            public bool LessThan(byte left, byte right) => left < right;

            public byte Not(byte value) => (byte)~value;

            public byte Or(byte left, byte right) => (byte)(left | right);

            public byte Subtract(byte left, byte right) => (byte)(left - right);

            public string ToHexString(byte value) => value.ToString("X2");

            public byte ToObject(long value) => (byte)value;

            public byte ToObject(ulong value) => (byte)value;

            public ulong ToUInt64(byte value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out byte result) => byte.TryParse(span, out result);
        }

        private struct SByteOperators : IUnderlyingOperators<sbyte>
        {
            public sbyte One => 1;

            public sbyte Zero => 0;

            public string OverflowMessage => SR.Overflow_SByte;

            public sbyte And(sbyte left, sbyte right) => (sbyte)(left & right);

            public sbyte LeftShift(sbyte value, int amount) => (sbyte)(value << amount);

            public bool LessThan(sbyte left, sbyte right) => left < right;

            public sbyte Not(sbyte value) => (sbyte)~value;

            public sbyte Or(sbyte left, sbyte right) => (sbyte)(left | right);

            public sbyte Subtract(sbyte left, sbyte right) => (sbyte)(left - right);

            public string ToHexString(sbyte value) => value.ToString("X2");

            public sbyte ToObject(long value) => (sbyte)value;

            public sbyte ToObject(ulong value) => (sbyte)value;

            public ulong ToUInt64(sbyte value) => (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out sbyte result) => sbyte.TryParse(span, out result);
        }

        private struct Int16Operators : IUnderlyingOperators<short>
        {
            public short One => 1;

            public short Zero => 0;

            public string OverflowMessage => SR.Overflow_Int16;

            public short And(short left, short right) => (short)(left & right);

            public short LeftShift(short value, int amount) => (short)(value << amount);

            public bool LessThan(short left, short right) => left < right;

            public short Not(short value) => (short)~value;

            public short Or(short left, short right) => (short)(left | right);

            public short Subtract(short left, short right) => (short)(left - right);

            public string ToHexString(short value) => value.ToString("X4");

            public short ToObject(long value) => (short)value;

            public short ToObject(ulong value) => (short)value;

            public ulong ToUInt64(short value) => (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out short result) => short.TryParse(span, out result);
        }

        private struct UInt16Operators : IUnderlyingOperators<ushort>
        {
            public ushort One => 1;

            public ushort Zero => 0;

            public string OverflowMessage => SR.Overflow_UInt16;

            public ushort And(ushort left, ushort right) => (ushort)(left & right);

            public ushort LeftShift(ushort value, int amount) => (ushort)(value << amount);

            public bool LessThan(ushort left, ushort right) => left < right;

            public ushort Not(ushort value) => (ushort)~value;

            public ushort Or(ushort left, ushort right) => (ushort)(left | right);

            public ushort Subtract(ushort left, ushort right) => (ushort)(left - right);

            public string ToHexString(ushort value) => value.ToString("X4");

            public ushort ToObject(long value) => (ushort)value;

            public ushort ToObject(ulong value) => (ushort)value;

            public ulong ToUInt64(ushort value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out ushort result) => ushort.TryParse(span, out result);
        }

        private struct Int32Operators : IUnderlyingOperators<int>
        {
            public int One => 1;

            public int Zero => 0;

            public string OverflowMessage => SR.Overflow_Int32;

            public int And(int left, int right) => left & right;

            public int LeftShift(int value, int amount) => value << amount;

            public bool LessThan(int left, int right) => left < right;

            public int Not(int value) => ~value;

            public int Or(int left, int right) => left | right;

            public int Subtract(int left, int right) => left - right;

            public string ToHexString(int value) => value.ToString("X8");

            public int ToObject(long value) => (int)value;

            public int ToObject(ulong value) => (int)value;

            public ulong ToUInt64(int value) => (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out int result) => int.TryParse(span, out result);
        }

        private struct UInt32Operators : IUnderlyingOperators<uint>
        {
            public uint One => 1;

            public uint Zero => 0;

            public string OverflowMessage => SR.Overflow_UInt32;

            public uint And(uint left, uint right) => left & right;

            public uint LeftShift(uint value, int amount) => value << amount;

            public bool LessThan(uint left, uint right) => left < right;

            public uint Not(uint value) => ~value;

            public uint Or(uint left, uint right) => left | right;

            public uint Subtract(uint left, uint right) => left - right;

            public string ToHexString(uint value) => value.ToString("X8");

            public uint ToObject(long value) => (uint)value;

            public uint ToObject(ulong value) => (uint)value;

            public ulong ToUInt64(uint value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out uint result) => uint.TryParse(span, out result);
        }

        private struct Int64Operators : IUnderlyingOperators<long>
        {
            public long One => 1;

            public long Zero => 0;

            public string OverflowMessage => SR.Overflow_Int64;

            public long And(long left, long right) => left & right;

            public long LeftShift(long value, int amount) => value << amount;

            public bool LessThan(long left, long right) => left < right;

            public long Not(long value) => ~value;

            public long Or(long left, long right) => left | right;

            public long Subtract(long left, long right) => left - right;

            public string ToHexString(long value) => value.ToString("X16");

            public long ToObject(long value) => value;

            public long ToObject(ulong value) => (long)value;

            public ulong ToUInt64(long value) => (ulong)value;

            public bool TryParse(ReadOnlySpan<char> span, out long result) => long.TryParse(span, out result);
        }

        private struct UInt64Operators : IUnderlyingOperators<ulong>
        {
            public ulong One => 1;

            public ulong Zero => 0;

            public string OverflowMessage => SR.Overflow_UInt64;

            public ulong And(ulong left, ulong right) => left & right;

            public ulong LeftShift(ulong value, int amount) => value << amount;

            public bool LessThan(ulong left, ulong right) => left < right;

            public ulong Not(ulong value) => ~value;

            public ulong Or(ulong left, ulong right) => left | right;

            public ulong Subtract(ulong left, ulong right) => left - right;

            public string ToHexString(ulong value) => value.ToString("X16");

            public ulong ToObject(long value) => (ulong)value;

            public ulong ToObject(ulong value) => value;

            public ulong ToUInt64(ulong value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out ulong result) => ulong.TryParse(span, out result);
        }

        private struct BooleanOperators : IUnderlyingOperators<bool>
        {
            public bool One => true;

            public bool Zero => false;

            public string OverflowMessage => null;

            public bool And(bool left, bool right) => left & right;

            public bool LeftShift(bool value, int amount) => !value;

            public bool LessThan(bool left, bool right) => !left & right;

            public bool Not(bool value) => !value;

            public bool Or(bool left, bool right) => left | right;

            public bool Subtract(bool left, bool right) => left ^ right;

            public string ToHexString(bool value) => Convert.ToByte(value).ToString("X2");

            public bool ToObject(long value) => value != 0L;

            public bool ToObject(ulong value) => value != 0UL;

            public ulong ToUInt64(bool value) => Convert.ToUInt64(value);

            public bool TryParse(ReadOnlySpan<char> span, out bool result) => bool.TryParse(span, out result);
        }

        private struct CharOperators : IUnderlyingOperators<char>
        {
            public char One => (char)1;

            public char Zero => (char)0;

            public string OverflowMessage => SR.Overflow_Char;

            public char And(char left, char right) => (char)(left & right);

            public char LeftShift(char value, int amount) => (char)(value << amount);

            public bool LessThan(char left, char right) => left < right;

            public char Not(char value) => (char)~value;

            public char Or(char left, char right) => (char)(left | right);

            public char Subtract(char left, char right) => (char)(left - right);

            public string ToHexString(char value) => ((ushort)value).ToString("X4");

            public char ToObject(long value) => (char)value;

            public char ToObject(ulong value) => (char)value;

            public ulong ToUInt64(char value) => value;

            public bool TryParse(ReadOnlySpan<char> span, out char result)
            {
                bool success = span.Length == 1;
                result = success ? span[0] : default;
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
        public bool HasFlag(Enum flag) => Bridge.HasAllFlags(this, flag);
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
        public static object ToObject(Type enumType, sbyte value) => GetBridge(enumType).ToObject(value);

        [CLSCompliant(false)]
        public static TEnum ToObject<TEnum>(sbyte value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);

        public static object ToObject(Type enumType, short value) => GetBridge(enumType).ToObject(value);
        
        public static TEnum ToObject<TEnum>(short value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);

        public static object ToObject(Type enumType, int value) => GetBridge(enumType).ToObject(value);
        
        public static TEnum ToObject<TEnum>(int value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);

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

        public static object ToObject(Type enumType, long value) => GetBridge(enumType).ToObject(value);
        
        public static TEnum ToObject<TEnum>(long value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);

        [CLSCompliant(false)]
        public static object ToObject(Type enumType, ulong value) => GetBridge(enumType).ToObject(value);

        [CLSCompliant(false)]
        public static TEnum ToObject<TEnum>(ulong value) where TEnum : struct, Enum => EnumBridge<TEnum>.Bridge.ToObject(value);
        #endregion
    }
}
