#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnString.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunctionMySql.h>
#include <Functions/GatherUtils/Algorithms.h>
#include <Functions/GatherUtils/Sinks.h>
#include <Functions/GatherUtils/Sources.h>
#include <common/bit_cast.h>
#include <DataTypes/DataTypeFixedString.h>
#include <DataTypes/DataTypeString.h>

namespace DB
{
using namespace GatherUtils;

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int TOO_LARGE_STRING_SIZE;
    extern const int INDEX_OF_POSITIONAL_ARGUMENT_IS_OUT_OF_RANGE;
}

namespace
{
    /// The maximum new padded length.
    constexpr ssize_t MAX_NEW_LENGTH = 1000000;

    /// Appends padding characters to a sink based on a pad string.
    /// Depending on how many padding characters are required to add
    /// the pad string can be copied only partly or be repeated multiple times.
    template <bool is_utf8>
    class PaddingChars
    {
    public:
        explicit PaddingChars(const String & pad_string_) : pad_string(pad_string_) { init(); }

        ALWAYS_INLINE size_t numCharsInPadString() const
        {
            if constexpr (is_utf8)
                return utf8_offsets.size() - 1;
            else
                return pad_string.length();
        }

        ALWAYS_INLINE size_t numCharsToNumBytes(size_t count) const
        {
            if constexpr (is_utf8)
                return utf8_offsets[count];
            else
                return count;
        }

        void appendTo(StringSink & res_sink, size_t num_chars) const
        {
            if (!num_chars)
                return;

            const size_t step = numCharsInPadString();
            while (true)
            {
                if (num_chars <= step)
                {
                    writeSlice(StringSource::Slice{bit_cast<const UInt8 *>(pad_string.data()), numCharsToNumBytes(num_chars)}, res_sink);
                    break;
                }
                writeSlice(StringSource::Slice{bit_cast<const UInt8 *>(pad_string.data()), numCharsToNumBytes(step)}, res_sink);
                num_chars -= step;
            }
        }

    private:
        void init()
        {
            if (pad_string.empty())
                pad_string = " ";

            if constexpr (is_utf8)
            {
                size_t offset = 0;
                utf8_offsets.reserve(pad_string.length() + 1);
                while (true)
                {
                    utf8_offsets.push_back(offset);
                    if (offset == pad_string.length())
                        break;
                    offset += UTF8::seqLength(pad_string[offset]);
                    if (offset > pad_string.length())
                        offset = pad_string.length();
                }
            }

            /// Not necessary, but good for performance.
            while (numCharsInPadString() < 16)
            {
                pad_string += pad_string;
                if constexpr (is_utf8)
                {
                    size_t old_size = utf8_offsets.size();
                    utf8_offsets.reserve((old_size - 1) * 2);
                    size_t base = utf8_offsets.back();
                    for (size_t i = 1; i != old_size; ++i)
                        utf8_offsets.push_back(utf8_offsets[i] + base);
                }
            }
        }

        String pad_string;
        std::vector<size_t> utf8_offsets;
    };

    /// Returns the number of characters in a slice.
    template <bool is_utf8>
    inline ALWAYS_INLINE size_t getLengthOfSlice(const StringSource::Slice & slice)
    {
        if constexpr (is_utf8)
            return UTF8::countCodePoints(slice.data, slice.size);
        else
            return slice.size;
    }

    /// Moves the end of a slice back by n characters.
    template <bool is_utf8>
    inline ALWAYS_INLINE StringSource::Slice removeSuffixFromSlice(const StringSource::Slice & slice, size_t suffix_length)
    {
        StringSource::Slice res = slice;
        if constexpr (is_utf8)
            res.size = UTF8StringSource::skipCodePointsBackward(slice.data + slice.size, suffix_length, slice.data) - res.data;
        else
            res.size -= std::min(suffix_length, res.size);
        return res;
    }

    /// If `is_right_pad` - it's the rightPad() function instead of leftPad().
    /// If `is_utf8` - lengths are measured in code points instead of bytes.
    template <bool is_right_pad, bool is_utf8, bool is_space_func = false>
    class FunctionPadString : public IFunction
    {
    public:
        static constexpr auto name = is_space_func ? "space" : (is_right_pad ? (is_utf8 ? "rightPadUTF8" : "rightPad") : (is_utf8 ? "leftPadUTF8" : "leftPad"));
        static FunctionPtr create(ContextPtr context)
        {
            if (context && context->getSettingsRef().enable_implicit_arg_type_convert)
                return std::make_shared<IFunctionMySql>(std::make_unique<FunctionPadString>());
            return std::make_shared<FunctionPadString>();
        }

        ArgType getArgumentsType() const override { return is_space_func ? ArgType::UINTS : ArgType::STR_UINT_STR; }

        String getName() const override { return name; }

        bool isVariadic() const override { return !is_space_func; }
        size_t getNumberOfArguments() const override { return !is_space_func ? 0 : 1; }

        bool useDefaultImplementationForConstants() const override { return true; }
        ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {2}; }

        DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
        {
            size_t number_of_arguments = arguments.size();

            if constexpr (is_space_func)
            {
                if (number_of_arguments != 1)
                    throw Exception(
                        ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                        "Number of arguments for function {} doesn't match: passed {}, should be 1",
                        getName(),
                        std::to_string(number_of_arguments));

                if (!isUnsignedInteger(arguments[0]))
                    throw Exception(
                        ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                        "Illegal type {} of the first argument of function {}, should be unsigned integer",
                        arguments[0]->getName(),
                        getName());

                return std::make_shared<DataTypeString>();
            }

            if (number_of_arguments != 2 && number_of_arguments != 3)
                throw Exception(
                    ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                    "Number of arguments for function {} doesn't match: passed {}, should be 2 or 3",
                    getName(),
                    std::to_string(number_of_arguments));

            if (!isStringOrFixedString(arguments[0]))
                throw Exception(
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Illegal type {} of the first argument of function {}, should be string",
                    arguments[0]->getName(),
                    getName());

            if (!isInteger(arguments[1]))
                throw Exception(
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Illegal type {} of the second argument of function {}, should be unsigned integer",
                    arguments[1]->getName(),
                    getName());

            if (number_of_arguments == 3 && !isStringOrFixedString(arguments[2]))
                throw Exception(
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Illegal type {} of the third argument of function {}, should be const string",
                    arguments[2]->getName(),
                    getName());

            return arguments[0];
        }

        ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
        {
            ColumnPtr column_string;
            ColumnPtr column_length;

            if constexpr (is_space_func)
            {
                column_string = DataTypeString().createColumn()->cloneResized(input_rows_count);
                column_length = arguments[0].column;
            }
            else
            {
                column_string = arguments[0].column;
                column_length = arguments[1].column;
            }

            String pad_string;
            if (arguments.size() == 3)
            {
                auto column_pad = arguments[2].column;
                const ColumnConst * column_pad_const = checkAndGetColumnConst<ColumnString>(column_pad.get());
                if (!column_pad_const)
                    throw Exception(
                        ErrorCodes::ILLEGAL_COLUMN,
                        "Illegal column {}, third argument of function {} must be a constant string",
                        column_pad->getName(),
                        getName());

                pad_string = column_pad_const->getValue<String>();
            }
            PaddingChars<is_utf8> padding_chars{pad_string};

            auto col_res = ColumnString::create();
            StringSink res_sink{*col_res, input_rows_count};

            if (const ColumnString * col = checkAndGetColumn<ColumnString>(column_string.get()))
                executeForSource(StringSource{*col}, column_length, padding_chars, res_sink);
            else if (const ColumnFixedString * col_fixed = checkAndGetColumn<ColumnFixedString>(column_string.get()))
                executeForSource(FixedStringSource{*col_fixed}, column_length, padding_chars, res_sink);
            else if (const ColumnConst * col_const = checkAndGetColumnConst<ColumnString>(column_string.get()))
                executeForSource(ConstSource<StringSource>{*col_const}, column_length, padding_chars, res_sink);
            else if (const ColumnConst * col_const_fixed = checkAndGetColumnConst<ColumnFixedString>(column_string.get()))
                executeForSource(ConstSource<FixedStringSource>{*col_const_fixed}, column_length, padding_chars, res_sink);
            else
                throw Exception(
                    ErrorCodes::ILLEGAL_COLUMN,
                    "Illegal column {}, first argument of function {} must be a string",
                    arguments[0].column->getName(),
                    getName());

            return col_res;
        }

    private:
        template <typename SourceStrings>
        void executeForSource(
            SourceStrings && strings,
            const ColumnPtr & column_length,
            const PaddingChars<is_utf8> & padding_chars,
            StringSink & res_sink) const
        {
            if (const auto * col_const = checkAndGetColumn<ColumnConst>(column_length.get()))
                executeForSourceAndLength(std::forward<SourceStrings>(strings), ConstSource<GenericValueSource>{*col_const}, padding_chars, res_sink);
            else
                executeForSourceAndLength(std::forward<SourceStrings>(strings), GenericValueSource{*column_length}, padding_chars, res_sink);
        }

        template <typename SourceStrings, typename SourceLengths>
        void executeForSourceAndLength(
            SourceStrings && strings,
            SourceLengths && lengths,
            const PaddingChars<is_utf8> & padding_chars,
            StringSink & res_sink) const
        {
            bool is_const_length = lengths.isConst();
            bool need_check_length = true;

            for (; !res_sink.isEnd(); res_sink.next(), strings.next(), lengths.next())
            {
                auto str = strings.getWhole();
                ssize_t current_length = getLengthOfSlice<is_utf8>(str);

                auto new_length_slice = lengths.getWhole();
                ssize_t new_length = new_length_slice.elements->getInt(new_length_slice.position);

                if (need_check_length)
                {
                    if (new_length > MAX_NEW_LENGTH)
                    {
                        throw Exception(
                            "New padded length (" + std::to_string(new_length) + ") is too big, maximum is: " + std::to_string(MAX_NEW_LENGTH),
                            ErrorCodes::TOO_LARGE_STRING_SIZE);
                    }
                    if (new_length < 0)
                    {
                        throw Exception(
                            ErrorCodes::INDEX_OF_POSITIONAL_ARGUMENT_IS_OUT_OF_RANGE, "New padded length ({}) is negative", std::to_string(new_length));
                    }
                    if (is_const_length)
                    {
                        size_t rows_count = res_sink.offsets.size();
                        res_sink.reserve((new_length + 1 /* zero terminator */) * rows_count);
                        need_check_length = false;
                    }
                }

                if (new_length == current_length)
                {
                    writeSlice(str, res_sink);
                }
                else if (new_length < current_length)
                {
                    str = removeSuffixFromSlice<is_utf8>(str, current_length - new_length);
                    writeSlice(str, res_sink);
                }
                else if (new_length > current_length)
                {
                    if constexpr (!is_right_pad)
                        padding_chars.appendTo(res_sink, new_length - current_length);

                    writeSlice(str, res_sink);

                    if constexpr (is_right_pad)
                        padding_chars.appendTo(res_sink, new_length - current_length);
                }
            }
        }
    };
}

REGISTER_FUNCTION(PadString)
{
    factory.registerFunction<FunctionPadString<false, false>>(); /// leftPad
    factory.registerFunction<FunctionPadString<false, true>>();  /// leftPadUTF8
    factory.registerFunction<FunctionPadString<true, false>>();  /// rightPad
    factory.registerFunction<FunctionPadString<true, true>>();   /// rightPadUTF8
    factory.registerFunction<FunctionPadString<false, false, true>>(FunctionFactory::CaseInsensitive);   /// space

    factory.registerAlias("lpad", "leftPad", FunctionFactory::CaseInsensitive);
    factory.registerAlias("rpad", "rightPad", FunctionFactory::CaseInsensitive);
}

}
