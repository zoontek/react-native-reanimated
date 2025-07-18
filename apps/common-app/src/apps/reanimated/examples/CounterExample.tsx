import React from 'react';
import { Button, StyleSheet, TextInput, View } from 'react-native';
import Animated, {
  useAnimatedProps,
  useDerivedValue,
  useSharedValue,
  withTiming,
} from 'react-native-reanimated';

const AnimatedTextInput = Animated.createAnimatedComponent(TextInput);

export default function CounterExample() {
  const ref = React.useRef(0);

  const sv = useSharedValue(0);

  const text = useDerivedValue(() => {
    return Math.round(sv.value * 100).toString();
  });

  const animatedProps = useAnimatedProps(() => {
    return { text: text.value, defaultValue: text.value };
  });

  const handleToggle = () => {
    ref.current = 1 - ref.current;
    sv.value = withTiming(ref.current, { duration: 1000 });
  };

  return (
    <>
      <View style={styles.buttons}>
        <Button onPress={handleToggle} title="Toggle" />
      </View>
      <AnimatedTextInput
        animatedProps={animatedProps}
        style={styles.text}
        editable={false}
      />
    </>
  );
}

const styles = StyleSheet.create({
  buttons: {
    marginVertical: 50,
  },
  text: {
    fontSize: 100,
    fontVariant: ['tabular-nums'],
    textAlign: 'center',
  },
});
