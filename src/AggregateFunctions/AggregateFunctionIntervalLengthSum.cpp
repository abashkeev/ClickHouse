#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/Helpers.h>
#include <DataTypes/DataTypeDate.h>

#include <unordered_set>

#include <AggregateFunctions/Combinators/AggregateFunctionNull.h>

#include <Columns/ColumnsNumber.h>

#include <Common/assert_cast.h>
#include <DataTypes/DataTypesNumber.h>

#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int TOO_LARGE_ARRAY_SIZE;
}

struct Settings;

namespace
{

/** Calculate total length of intervals without intersections. Each interval is the pair of numbers [begin, end];
  * Returns UInt64 for integral types (UInt/Int*, Date/DateTime) and returns Float64 for Float*.
  *
  * Implementation simply stores intervals sorted by beginning and sums lengths at final.
  */
template <typename T>
struct AggregateFunctionIntervalLengthSumData
{
    constexpr static size_t MAX_ARRAY_SIZE = 0xFFFFFF;

    using Segment = std::pair<T, T>;
    using Segments = PODArrayWithStackMemory<Segment, 64>;

    bool sorted = false;

    Segments segments;

    void add(T begin, T end)
    {
        /// Reversed intervals are counted by absolute value of their length.
        if (unlikely(end < begin))
            std::swap(begin, end);
        else if (unlikely(begin == end))
            return;

        if (sorted && !segments.empty())
            sorted = segments.back().first <= begin;
        segments.emplace_back(begin, end);
    }

    void merge(const AggregateFunctionIntervalLengthSumData & other)
    {
        if (other.segments.empty())
            return;

        const auto size = segments.size();

        segments.insert(std::begin(other.segments), std::end(other.segments));

        /// either sort whole container or do so partially merging ranges afterwards
        if (!sorted && !other.sorted)
        {
            ::sort(std::begin(segments), std::end(segments));
        }
        else
        {
            const auto begin = std::begin(segments);
            const auto middle = std::next(begin, size);
            const auto end = std::end(segments);

            if (!sorted)
                ::sort(begin, middle);

            if (!other.sorted)
                ::sort(middle, end);

            std::inplace_merge(begin, middle, end);
        }

        sorted = true;
    }

    void sort()
    {
        if (sorted)
            return;

        ::sort(std::begin(segments), std::end(segments));
        sorted = true;
    }

    void serialize(WriteBuffer & buf) const
    {
        writeBinary(sorted, buf);
        writeBinary(segments.size(), buf);

        for (const auto & time_gap : segments)
        {
            writeBinary(time_gap.first, buf);
            writeBinary(time_gap.second, buf);
        }
    }

    void deserialize(ReadBuffer & buf)
    {
        readBinary(sorted, buf);

        size_t size;
        readBinary(size, buf);

        if (unlikely(size > MAX_ARRAY_SIZE))
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE, "Too large array size (maximum: {})", MAX_ARRAY_SIZE);

        segments.clear();
        segments.reserve(size);

        Segment segment;
        for (size_t i = 0; i < size; ++i)
        {
            readBinary(segment.first, buf);
            readBinary(segment.second, buf);
            segments.emplace_back(segment);
        }
    }
};

template <typename T, typename Data>
class AggregateFunctionIntervalLengthSum final : public IAggregateFunctionDataHelper<Data, AggregateFunctionIntervalLengthSum<T, Data>>
{
private:
    static auto NO_SANITIZE_UNDEFINED length(typename Data::Segment segment)
    {
        return segment.second - segment.first;
    }

    template <typename TResult>
    TResult getIntervalLengthSum(Data & data) const
    {
        if (data.segments.empty())
            return 0;

        data.sort();

        TResult res = 0;

        typename Data::Segment curr_segment = data.segments[0];

        for (size_t i = 1, size = data.segments.size(); i < size; ++i)
        {
            const typename Data::Segment & next_segment = data.segments[i];

            /// Check if current interval intersects with next one then add length, otherwise advance interval end.
            if (curr_segment.second < next_segment.first)
            {
                res += length(curr_segment);
                curr_segment = next_segment;
            }
            else if (next_segment.second > curr_segment.second)
            {
                curr_segment.second = next_segment.second;
            }
        }
        res += length(curr_segment);

        return res;
    }

public:
    String getName() const override { return "intervalLengthSum"; }

    explicit AggregateFunctionIntervalLengthSum(const DataTypes & arguments)
        : IAggregateFunctionDataHelper<Data, AggregateFunctionIntervalLengthSum<T, Data>>(arguments, {}, createResultType())
    {
    }

    static DataTypePtr createResultType()
    {
        if constexpr (is_floating_point<T>)
            return std::make_shared<DataTypeFloat64>();
        return std::make_shared<DataTypeUInt64>();
    }

    bool allocatesMemoryInArena() const override { return false; }

    AggregateFunctionPtr getOwnNullAdapter(
        const AggregateFunctionPtr & nested_function,
        const DataTypes & arguments,
        const Array & params,
        const AggregateFunctionProperties & /*properties*/) const override
    {
        return std::make_shared<AggregateFunctionNullVariadic<false, false>>(nested_function, arguments, params);
    }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, const size_t row_num, Arena *) const override
    {
        auto begin = assert_cast<const ColumnVector<T> *>(columns[0])->getData()[row_num];
        auto end = assert_cast<const ColumnVector<T> *>(columns[1])->getData()[row_num];
        this->data(place).add(begin, end);
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).merge(this->data(rhs));
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        this->data(place).serialize(buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena *) const override
    {
        this->data(place).deserialize(buf);
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        if constexpr (is_floating_point<T>)
            assert_cast<ColumnFloat64 &>(to).getData().push_back(getIntervalLengthSum<Float64>(this->data(place)));
        else
            assert_cast<ColumnUInt64 &>(to).getData().push_back(getIntervalLengthSum<UInt64>(this->data(place)));
    }
};


template <template <typename> class Data>
AggregateFunctionPtr
createAggregateFunctionIntervalLengthSum(const std::string & name, const DataTypes & arguments, const Array &, const Settings *)
{
    if (arguments.size() != 2)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Aggregate function {} requires two timestamps argument.", name);

    auto args = {arguments[0].get(), arguments[1].get()};

    if (WhichDataType{args.begin()[0]}.idx != WhichDataType{args.begin()[1]}.idx)
        throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                        "Illegal types {} and {} of arguments "
                        "of aggregate function {}, both arguments should have same data type",
                        args.begin()[0]->getName(), args.begin()[1]->getName(), name);

    for (const auto & arg : args)
    {
        if (!isNativeNumber(arg) && !isDate(arg) && !isDateTime(arg))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                            "Illegal type {} of argument of aggregate function {}, must "
                            "be native integral type, Date/DateTime or Float", arg->getName(), name);
    }

    AggregateFunctionPtr res(createWithBasicNumberOrDateOrDateTime<AggregateFunctionIntervalLengthSum, Data>(*arguments[0], arguments));

    if (res)
        return res;

    throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Illegal type {} of argument of aggregate function {}, must "
                    "be native integral type, Date/DateTime or Float", arguments.front().get()->getName(), name);
}

}

void registerAggregateFunctionIntervalLengthSum(AggregateFunctionFactory & factory)
{
    factory.registerFunction("intervalLengthSum", createAggregateFunctionIntervalLengthSum<AggregateFunctionIntervalLengthSumData>);
}

}
