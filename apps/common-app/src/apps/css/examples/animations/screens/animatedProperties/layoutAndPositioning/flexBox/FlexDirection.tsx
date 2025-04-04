import { StyleSheet, View } from 'react-native';
import Animated from 'react-native-reanimated';

import { ExamplesScreen, VerticalExampleCard } from '@/apps/css/components';
import { colors, radius, sizes } from '@/theme';

const BOX_COLORS = [colors.primaryLight, colors.primary, colors.primaryDark];

export default function FlexDirection() {
  return (
    <ExamplesScreen
      CardComponent={VerticalExampleCard}
      buildAnimation={() => ({
        animationDuration: '4s',
        animationIterationCount: 'infinite',
        animationName: {
          '0%, 100%': {
            flexDirection: 'column',
          },
          '25%': {
            flexDirection: 'row',
          },
          '50%': {
            flexDirection: 'column-reverse',
          },
          '75%': {
            flexDirection: 'row-reverse',
          },
        },
        animationTimingFunction: 'linear',
      })}
      renderExample={({ animation }) => (
        <Animated.View style={[StyleSheet.absoluteFill, animation]}>
          {BOX_COLORS.map((color, index) => (
            <View
              key={index}
              style={[styles.box, { backgroundColor: color }]}
            />
          ))}
        </Animated.View>
      )}
      sections={[
        {
          examples: [
            {
              description:
                "`flexDirection` is a **discrete** property. That means, it **can't be smoothly animated** between values. However, we can still change this property in the animation keyframes but the change will be **abrupt**.",
              minExampleHeight: sizes.xxxl,
              title: 'Changing Flex Direction',
            },
          ],
          title: 'Flex Direction',
        },
      ]}
    />
  );
}

const styles = StyleSheet.create({
  box: {
    backgroundColor: colors.primary,
    borderRadius: radius.sm,
    height: sizes.md,
    width: sizes.md,
  },
});
